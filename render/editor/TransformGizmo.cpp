#include "render/editor/TransformGizmo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace demo::editor {
namespace {

constexpr double kMathEpsilon = 1.0e-12;
constexpr double kGeneralMatrixConditionLimit = 1.0e12;

[[nodiscard]] bool finite(double value) noexcept {
  return std::isfinite(value);
}

[[nodiscard]] bool finite(OverlayVec2 value) noexcept {
  return finite(value.x) && finite(value.y);
}

[[nodiscard]] bool finite(OverlayQuaternion value) noexcept {
  return finite(value.x) && finite(value.y) && finite(value.z) &&
         finite(value.w);
}

[[nodiscard]] OverlayVec3 add(OverlayVec3 left, OverlayVec3 right) noexcept {
  return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] OverlayVec3 subtract(OverlayVec3 left,
                                   OverlayVec3 right) noexcept {
  return {left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] OverlayVec3 multiply(OverlayVec3 value, double scalar) noexcept {
  return {value.x * scalar, value.y * scalar, value.z * scalar};
}

[[nodiscard]] double dot(OverlayVec3 left, OverlayVec3 right) noexcept {
  return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] OverlayVec3 cross(OverlayVec3 left, OverlayVec3 right) noexcept {
  return {left.y * right.z - left.z * right.y,
          left.z * right.x - left.x * right.z,
          left.x * right.y - left.y * right.x};
}

[[nodiscard]] std::optional<OverlayVec3>
normalized(OverlayVec3 value) noexcept {
  if (!finiteOverlayVector(value)) {
    return std::nullopt;
  }
  const double largest =
      std::max({std::abs(value.x), std::abs(value.y), std::abs(value.z)});
  if (!finite(largest) || largest <= kMathEpsilon) {
    return std::nullopt;
  }
  const OverlayVec3 scaled = multiply(value, 1.0 / largest);
  const double lengthSquared = dot(scaled, scaled);
  if (!finite(lengthSquared) || lengthSquared <= kMathEpsilon * kMathEpsilon) {
    return std::nullopt;
  }
  const double inverseLength = 1.0 / std::sqrt(lengthSquared);
  const OverlayVec3 result = multiply(scaled, inverseLength);
  return finiteOverlayVector(result) ? std::optional<OverlayVec3>(result)
                                     : std::nullopt;
}

[[nodiscard]] std::optional<OverlayQuaternion>
normalized(OverlayQuaternion value) noexcept {
  if (!finite(value)) {
    return std::nullopt;
  }
  const double largest = std::max(
      {std::abs(value.x), std::abs(value.y), std::abs(value.z),
       std::abs(value.w)});
  if (!finite(largest) || largest <= kMathEpsilon) {
    return std::nullopt;
  }
  const OverlayQuaternion scaled{value.x / largest, value.y / largest,
                                 value.z / largest, value.w / largest};
  const double lengthSquared = scaled.x * scaled.x + scaled.y * scaled.y +
                               scaled.z * scaled.z + scaled.w * scaled.w;
  if (!finite(lengthSquared) || lengthSquared <= kMathEpsilon * kMathEpsilon) {
    return std::nullopt;
  }
  const double inverseLength = 1.0 / std::sqrt(lengthSquared);
  const OverlayQuaternion result{scaled.x * inverseLength,
                                 scaled.y * inverseLength,
                                 scaled.z * inverseLength,
                                 scaled.w * inverseLength};
  return finite(result) ? std::optional<OverlayQuaternion>(result)
                        : std::nullopt;
}

[[nodiscard]] OverlayQuaternion multiply(OverlayQuaternion left,
                                         OverlayQuaternion right) noexcept {
  return {
      left.w * right.x + left.x * right.w + left.y * right.z -
          left.z * right.y,
      left.w * right.y - left.x * right.z + left.y * right.w +
          left.z * right.x,
      left.w * right.z + left.x * right.y - left.y * right.x +
          left.z * right.w,
      left.w * right.w - left.x * right.x - left.y * right.y -
          left.z * right.z};
}

[[nodiscard]] std::optional<OverlayVec3>
rotate(OverlayQuaternion rotation, OverlayVec3 value) noexcept {
  const auto unit = normalized(rotation);
  if (!unit || !finiteOverlayVector(value)) {
    return std::nullopt;
  }
  const OverlayVec3 q{unit->x, unit->y, unit->z};
  const OverlayVec3 first = multiply(cross(q, value), 2.0);
  const OverlayVec3 result =
      add(value, add(multiply(first, unit->w), cross(q, first)));
  return finiteOverlayVector(result) ? std::optional<OverlayVec3>(result)
                                     : std::nullopt;
}

[[nodiscard]] OverlayQuaternion axisAngleQuaternion(OverlayVec3 axis,
                                                    double degrees) noexcept {
  const double radians = degrees * std::numbers::pi_v<double> / 180.0;
  const double half = radians * 0.5;
  const double sine = std::sin(half);
  return {axis.x * sine, axis.y * sine, axis.z * sine, std::cos(half)};
}

struct HomogeneousPoint final {
  double x{0.0};
  double y{0.0};
  double z{0.0};
  double w{0.0};
};

[[nodiscard]] std::optional<HomogeneousPoint>
transformHomogeneous(const OverlayMatrix4 &matrix, double x, double y,
                     double z, double w) noexcept {
  if (!finiteOverlayMatrix(matrix) || !finite(x) || !finite(y) || !finite(z) ||
      !finite(w)) {
    return std::nullopt;
  }
  const std::array<double, 4> input{x, y, z, w};
  HomogeneousPoint result;
  std::array<double *, 4> output{&result.x, &result.y, &result.z, &result.w};
  for (std::size_t row = 0; row < 4u; ++row) {
    double value = 0.0;
    for (std::size_t column = 0; column < 4u; ++column) {
      value += matrix.elements[row * 4u + column] * input[column];
    }
    if (!finite(value)) {
      return std::nullopt;
    }
    *output[row] = value;
  }
  return result;
}

struct LinearInverse final {
  std::array<double, 9> values{};
  double determinant{0.0};
  double condition{0.0};
};

[[nodiscard]] double matrix3InfinityNorm(
    const std::array<double, 9> &value) noexcept {
  double result = 0.0;
  for (std::size_t row = 0; row < 3u; ++row) {
    const double sum = std::abs(value[row * 3u]) +
                       std::abs(value[row * 3u + 1u]) +
                       std::abs(value[row * 3u + 2u]);
    result = std::max(result, sum);
  }
  return result;
}

[[nodiscard]] std::optional<LinearInverse>
parentLinearInverse(const OverlayMatrix4 &matrix) noexcept {
  if (!finiteOverlayMatrix(matrix)) {
    return std::nullopt;
  }
  constexpr double affineTolerance = kMathEpsilon;
  if (std::abs(matrix.elements[12]) > affineTolerance ||
      std::abs(matrix.elements[13]) > affineTolerance ||
      std::abs(matrix.elements[14]) > affineTolerance ||
      std::abs(matrix.elements[15] - 1.0) > affineTolerance) {
    return std::nullopt;
  }

  const std::array<double, 9> value{
      matrix.elements[0], matrix.elements[1], matrix.elements[2],
      matrix.elements[4], matrix.elements[5], matrix.elements[6],
      matrix.elements[8], matrix.elements[9], matrix.elements[10]};
  const double a = value[0];
  const double b = value[1];
  const double c = value[2];
  const double d = value[3];
  const double e = value[4];
  const double f = value[5];
  const double g = value[6];
  const double h = value[7];
  const double i = value[8];
  const double determinant =
      a * (e * i - f * h) - b * (d * i - f * g) +
      c * (d * h - e * g);
  const double norm = matrix3InfinityNorm(value);
  const double determinantTolerance =
      kMathEpsilon * std::max(1.0, norm * norm * norm);
  if (!finite(determinant) || std::abs(determinant) <= determinantTolerance) {
    return std::nullopt;
  }
  const double inverseDeterminant = 1.0 / determinant;
  const std::array<double, 9> inverse{
      (e * i - f * h) * inverseDeterminant,
      (c * h - b * i) * inverseDeterminant,
      (b * f - c * e) * inverseDeterminant,
      (f * g - d * i) * inverseDeterminant,
      (a * i - c * g) * inverseDeterminant,
      (c * d - a * f) * inverseDeterminant,
      (d * h - e * g) * inverseDeterminant,
      (b * g - a * h) * inverseDeterminant,
      (a * e - b * d) * inverseDeterminant};
  const double condition = norm * matrix3InfinityNorm(inverse);
  if (!finite(condition) || condition > kGizmoParentConditionLimit) {
    return std::nullopt;
  }
  for (double element : inverse) {
    if (!finite(element)) {
      return std::nullopt;
    }
  }
  return LinearInverse{inverse, determinant, condition};
}

[[nodiscard]] OverlayVec3 multiplyLinear(const OverlayMatrix4 &matrix,
                                         OverlayVec3 value) noexcept {
  return {matrix.elements[0] * value.x + matrix.elements[1] * value.y +
              matrix.elements[2] * value.z,
          matrix.elements[4] * value.x + matrix.elements[5] * value.y +
              matrix.elements[6] * value.z,
          matrix.elements[8] * value.x + matrix.elements[9] * value.y +
              matrix.elements[10] * value.z};
}

[[nodiscard]] OverlayVec3 multiplyLinear(
    const std::array<double, 9> &matrix, OverlayVec3 value) noexcept {
  return {matrix[0] * value.x + matrix[1] * value.y + matrix[2] * value.z,
          matrix[3] * value.x + matrix[4] * value.y + matrix[5] * value.z,
          matrix[6] * value.x + matrix[7] * value.y + matrix[8] * value.z};
}

[[nodiscard]] std::optional<std::size_t>
axisIndex(GizmoComponent component) noexcept {
  switch (component) {
  case GizmoComponent::translateX:
  case GizmoComponent::rotateX:
  case GizmoComponent::scaleX:
    return 0u;
  case GizmoComponent::translateY:
  case GizmoComponent::rotateY:
  case GizmoComponent::scaleY:
    return 1u;
  case GizmoComponent::translateZ:
  case GizmoComponent::rotateZ:
  case GizmoComponent::scaleZ:
    return 2u;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] OverlayVec3 canonicalAxis(std::size_t index) noexcept {
  switch (index) {
  case 0u:
    return {1.0, 0.0, 0.0};
  case 1u:
    return {0.0, 1.0, 0.0};
  default:
    return {0.0, 0.0, 1.0};
  }
}

[[nodiscard]] bool componentVisible(GizmoComponent component,
                                    GizmoVisibility visibility) noexcept {
  const std::size_t index = gizmoComponentIndex(component);
  if (index <= gizmoComponentIndex(GizmoComponent::translateYZ)) {
    return hasGizmoVisibility(visibility, GizmoVisibility::translation);
  }
  if (index <= gizmoComponentIndex(GizmoComponent::rotateZ)) {
    return hasGizmoVisibility(visibility, GizmoVisibility::rotation);
  }
  if (index < kGizmoComponentCount) {
    return hasGizmoVisibility(visibility, GizmoVisibility::scale);
  }
  return false;
}

[[nodiscard]] bool validVisibility(GizmoVisibility visibility) noexcept {
  return (static_cast<std::uint8_t>(visibility) &
          ~static_cast<std::uint8_t>(GizmoVisibility::all)) == 0u;
}

[[nodiscard]] bool validHitStyle(const GizmoHitStyle &style) noexcept {
  return finite(style.axisPickRadius) && style.axisPickRadius > 0.0 &&
         finite(style.translationPlaneTolerance) &&
         style.translationPlaneTolerance >= 0.0 &&
         finite(style.rotationRingTolerance) &&
         style.rotationRingTolerance > 0.0 &&
         finite(style.scaleAxisPickRadius) &&
         style.scaleAxisPickRadius > 0.0 &&
         finite(style.uniformCenterPickHalfExtent) &&
         style.uniformCenterPickHalfExtent > 0.0;
}

[[nodiscard]] std::optional<double>
rayAabb(const GizmoRay &ray, OverlayBounds bounds, double expansion) noexcept {
  if (!finiteOverlayVector(bounds.minimum) ||
      !finiteOverlayVector(bounds.maximum) || !finite(expansion) ||
      expansion < 0.0) {
    return std::nullopt;
  }
  bounds.minimum = subtract(bounds.minimum, {expansion, expansion, expansion});
  bounds.maximum = add(bounds.maximum, {expansion, expansion, expansion});
  double minimumT = 0.0;
  double maximumT = std::numeric_limits<double>::infinity();
  const std::array<double, 3> origin{ray.origin.x, ray.origin.y, ray.origin.z};
  const std::array<double, 3> direction{ray.direction.x, ray.direction.y,
                                        ray.direction.z};
  const std::array<double, 3> minimum{bounds.minimum.x, bounds.minimum.y,
                                      bounds.minimum.z};
  const std::array<double, 3> maximum{bounds.maximum.x, bounds.maximum.y,
                                      bounds.maximum.z};
  for (std::size_t axis = 0; axis < 3u; ++axis) {
    if (std::abs(direction[axis]) <= kMathEpsilon) {
      if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis]) {
        return std::nullopt;
      }
      continue;
    }
    double first = (minimum[axis] - origin[axis]) / direction[axis];
    double second = (maximum[axis] - origin[axis]) / direction[axis];
    if (first > second) {
      std::swap(first, second);
    }
    minimumT = std::max(minimumT, first);
    maximumT = std::min(maximumT, second);
    if (maximumT < minimumT) {
      return std::nullopt;
    }
  }
  return finite(minimumT) && maximumT >= 0.0
             ? std::optional<double>(minimumT)
             : std::nullopt;
}

struct RaySegmentDistance final {
  double rayT{0.0};
  double segmentT{0.0};
  double distance{0.0};
};

[[nodiscard]] std::optional<RaySegmentDistance>
closestRaySegment(const GizmoRay &ray, OverlayVec3 start,
                  OverlayVec3 end) noexcept {
  const OverlayVec3 segment = subtract(end, start);
  const double segmentLengthSquared = dot(segment, segment);
  if (!finite(segmentLengthSquared) || segmentLengthSquared <= kMathEpsilon) {
    return std::nullopt;
  }
  const double startProjection = dot(subtract(start, ray.origin), ray.direction);
  const double endProjection = dot(subtract(end, ray.origin), ray.direction);
  if (!finite(startProjection) || !finite(endProjection) ||
      std::max(startProjection, endProjection) < 0.0) {
    return std::nullopt;
  }
  const OverlayVec3 difference = subtract(ray.origin, start);
  const double b = dot(ray.direction, segment);
  const double d = dot(ray.direction, difference);
  const double e = dot(segment, difference);
  const double denominator = segmentLengthSquared - b * b;
  double rayT = 0.0;
  double segmentT = 0.0;
  if (std::abs(denominator) > kMathEpsilon * segmentLengthSquared) {
    rayT = (b * e - segmentLengthSquared * d) / denominator;
    segmentT = (e - b * d) / denominator;
  } else {
    segmentT = e / segmentLengthSquared;
  }
  segmentT = std::clamp(segmentT, 0.0, 1.0);
  rayT = std::max(0.0, b * segmentT - d);
  segmentT = std::clamp((b * rayT + e) / segmentLengthSquared, 0.0, 1.0);
  rayT = std::max(0.0, b * segmentT - d);
  const OverlayVec3 rayPoint = add(ray.origin, multiply(ray.direction, rayT));
  const OverlayVec3 segmentPoint = add(start, multiply(segment, segmentT));
  const OverlayVec3 separation = subtract(rayPoint, segmentPoint);
  const double distance = std::sqrt(std::max(0.0, dot(separation, separation)));
  if (!finite(rayT) || !finite(segmentT) || !finite(distance)) {
    return std::nullopt;
  }
  return RaySegmentDistance{rayT, segmentT, distance};
}

[[nodiscard]] GizmoColorToken colorToken(GizmoComponent component) noexcept {
  switch (component) {
  case GizmoComponent::translateX:
  case GizmoComponent::rotateX:
  case GizmoComponent::scaleX:
    return GizmoColorToken::xAxis;
  case GizmoComponent::translateY:
  case GizmoComponent::rotateY:
  case GizmoComponent::scaleY:
    return GizmoColorToken::yAxis;
  case GizmoComponent::translateZ:
  case GizmoComponent::rotateZ:
  case GizmoComponent::scaleZ:
    return GizmoColorToken::zAxis;
  case GizmoComponent::translateXY:
    return GizmoColorToken::xyPlane;
  case GizmoComponent::translateXZ:
    return GizmoColorToken::xzPlane;
  case GizmoComponent::translateYZ:
    return GizmoColorToken::yzPlane;
  default:
    return GizmoColorToken::uniform;
  }
}

[[nodiscard]] GizmoModelToken modelToken(GizmoComponent component) noexcept {
  const std::size_t index = gizmoComponentIndex(component);
  if (index <= gizmoComponentIndex(GizmoComponent::translateYZ)) {
    return GizmoModelToken::translate;
  }
  if (index <= gizmoComponentIndex(GizmoComponent::rotateZ)) {
    return GizmoModelToken::rotate;
  }
  return GizmoModelToken::scale;
}

[[nodiscard]] bool validComponentOrNone(GizmoComponent component) noexcept {
  return component == GizmoComponent::none ||
         gizmoComponentIndex(component) < kGizmoComponentCount;
}

[[nodiscard]] GizmoAttachmentFence
attachmentFence(const GizmoAttachment &attachment) noexcept {
  return {attachment.nodeId, attachment.documentRevision,
          attachment.selectionRevision};
}

[[nodiscard]] bool matchingFence(const GizmoInteractionState &state,
                                 const GizmoAttachment &attachment) noexcept {
  return validGizmoAttachment(attachment) &&
         state.fence == attachmentFence(attachment);
}

[[nodiscard]] OverlayGenerationBinding
generationBinding(const EditorViewportMetrics &metrics) noexcept {
  return {metrics.viewportGeneration, metrics.projection, metrics.view};
}

void fillActionIdentity(GizmoAction &action,
                        const GizmoInteractionState &state) noexcept {
  action.transactionId = state.activeTransactionId;
  action.nodeId = state.fence.nodeId;
  action.documentRevision = state.fence.documentRevision;
  action.selectionRevision = state.fence.selectionRevision;
  action.component = state.activeComponent;
  if (state.hasRetainedPreview) {
    action.translation = state.retainedPreview.translation;
    action.rotation = state.retainedPreview.rotation;
    action.scale = state.retainedPreview.scale;
  }
}

void appendAction(GizmoActionResolution &resolution,
                  GizmoAction action) noexcept {
  if (resolution.actionCount < resolution.actions.size()) {
    resolution.actions[resolution.actionCount++] = action;
  }
}

[[nodiscard]] bool validBatch(const GizmoTerminalBatch &batch) noexcept {
  if (batch.count > batch.events.size()) {
    return false;
  }
  std::uint64_t previous = 0;
  for (std::uint8_t index = 0; index < batch.count; ++index) {
    const GizmoTerminalEvent &event = batch.events[index];
    if (event.sequence == 0 || event.sequence <= previous) {
      return false;
    }
    previous = event.sequence;
    switch (event.kind) {
    case GizmoTerminalEventKind::deliveredRelease:
    case GizmoTerminalEventKind::escapePressed:
    case GizmoTerminalEventKind::focusLost:
    case GizmoTerminalEventKind::suppressedPhysicalMouseUp:
      break;
    default:
      return false;
    }
  }
  return true;
}

} // namespace

bool finiteOverlayVector(OverlayVec3 value) noexcept {
  return finite(value.x) && finite(value.y) && finite(value.z);
}

bool finiteOverlayMatrix(const OverlayMatrix4 &value) noexcept {
  return std::all_of(value.elements.begin(), value.elements.end(),
                     [](double element) { return finite(element); });
}

bool validGizmoAttachment(const GizmoAttachment &value) noexcept {
  // Revision zero is the valid clean baseline of a freshly loaded document;
  // mapping/selection generations still provide the attachment fence.
  return value.nodeId.valid() && value.selectionRevision != 0 &&
         finiteOverlayVector(value.local.translation) &&
         finite(value.local.rotation) && normalized(value.local.rotation) &&
         finiteOverlayVector(value.local.scale) &&
         parentLinearInverse(value.parentWorld).has_value();
}

std::optional<OverlayMatrix4>
invertOverlayMatrix(const OverlayMatrix4 &value) noexcept {
  if (!finiteOverlayMatrix(value)) {
    return std::nullopt;
  }
  std::array<std::array<double, 8>, 4> augmented{};
  double sourceNorm = 0.0;
  for (std::size_t row = 0; row < 4u; ++row) {
    double rowSum = 0.0;
    for (std::size_t column = 0; column < 4u; ++column) {
      augmented[row][column] = value.elements[row * 4u + column];
      rowSum += std::abs(augmented[row][column]);
      augmented[row][column + 4u] = row == column ? 1.0 : 0.0;
    }
    sourceNorm = std::max(sourceNorm, rowSum);
  }
  if (!finite(sourceNorm) || sourceNorm <= kMathEpsilon) {
    return std::nullopt;
  }

  for (std::size_t column = 0; column < 4u; ++column) {
    std::size_t pivot = column;
    for (std::size_t row = column + 1u; row < 4u; ++row) {
      if (std::abs(augmented[row][column]) >
          std::abs(augmented[pivot][column])) {
        pivot = row;
      }
    }
    if (std::abs(augmented[pivot][column]) <= kMathEpsilon * sourceNorm) {
      return std::nullopt;
    }
    if (pivot != column) {
      std::swap(augmented[pivot], augmented[column]);
    }
    const double divisor = augmented[column][column];
    for (double &element : augmented[column]) {
      element /= divisor;
    }
    for (std::size_t row = 0; row < 4u; ++row) {
      if (row == column) {
        continue;
      }
      const double factor = augmented[row][column];
      for (std::size_t item = 0; item < 8u; ++item) {
        augmented[row][item] -= factor * augmented[column][item];
      }
    }
  }

  OverlayMatrix4 inverse;
  double inverseNorm = 0.0;
  for (std::size_t row = 0; row < 4u; ++row) {
    double rowSum = 0.0;
    for (std::size_t column = 0; column < 4u; ++column) {
      const double element = augmented[row][column + 4u];
      if (!finite(element)) {
        return std::nullopt;
      }
      inverse.elements[row * 4u + column] = element;
      rowSum += std::abs(element);
    }
    inverseNorm = std::max(inverseNorm, rowSum);
  }
  const double condition = sourceNorm * inverseNorm;
  return finite(condition) && condition <= kGeneralMatrixConditionLimit
             ? std::optional<OverlayMatrix4>(inverse)
             : std::nullopt;
}

std::optional<OverlayVec3>
transformOverlayPoint(const OverlayMatrix4 &matrix,
                      OverlayVec3 point) noexcept {
  const auto transformed =
      transformHomogeneous(matrix, point.x, point.y, point.z, 1.0);
  if (!transformed || std::abs(transformed->w) <= kMathEpsilon) {
    return std::nullopt;
  }
  const OverlayVec3 result{transformed->x / transformed->w,
                           transformed->y / transformed->w,
                           transformed->z / transformed->w};
  return finiteOverlayVector(result) ? std::optional<OverlayVec3>(result)
                                     : std::nullopt;
}

std::optional<OverlayVec3>
transformOverlayDirection(const OverlayMatrix4 &matrix,
                          OverlayVec3 direction) noexcept {
  const auto transformed = transformHomogeneous(
      matrix, direction.x, direction.y, direction.z, 0.0);
  if (!transformed || std::abs(transformed->w) > kMathEpsilon) {
    return std::nullopt;
  }
  const OverlayVec3 result{transformed->x, transformed->y, transformed->z};
  return finiteOverlayVector(result) ? std::optional<OverlayVec3>(result)
                                     : std::nullopt;
}

std::optional<GizmoRay>
makeGizmoRay(LogicalViewportPoint pointer,
             const EditorViewportMetrics &metrics,
             const GizmoCameraSnapshot &camera) noexcept {
  if (!metrics.valid() || !finite(pointer.x) || !finite(pointer.y) ||
      camera.generation != generationBinding(metrics) ||
      !finiteOverlayMatrix(camera.inverseViewProjection)) {
    return std::nullopt;
  }
  const double u =
      (pointer.x - metrics.logical.origin.x) / metrics.logical.extent.width;
  const double v =
      (pointer.y - metrics.logical.origin.y) / metrics.logical.extent.height;
  if (!finite(u) || !finite(v)) {
    return std::nullopt;
  }
  double transformedU = u;
  double transformedV = v;
  switch (metrics.surfaceTransform) {
  case EditorViewportSurfaceTransform::identity:
    break;
  case EditorViewportSurfaceTransform::rotate90:
    transformedU = 1.0 - v;
    transformedV = u;
    break;
  case EditorViewportSurfaceTransform::rotate180:
    transformedU = 1.0 - u;
    transformedV = 1.0 - v;
    break;
  case EditorViewportSurfaceTransform::rotate270:
    transformedU = v;
    transformedV = 1.0 - u;
    break;
  case EditorViewportSurfaceTransform::mirrorHorizontal:
    transformedU = 1.0 - u;
    break;
  case EditorViewportSurfaceTransform::mirrorVertical:
    transformedV = 1.0 - v;
    break;
  case EditorViewportSurfaceTransform::mirrorHorizontalRotate90:
    transformedU = 1.0 - v;
    transformedV = 1.0 - u;
    break;
  case EditorViewportSurfaceTransform::mirrorHorizontalRotate270:
    transformedU = v;
    transformedV = u;
    break;
  default:
    return std::nullopt;
  }

  const double ndcX = transformedU * 2.0 - 1.0;
  const double ndcY = camera.ndcYDown ? transformedV * 2.0 - 1.0
                                     : 1.0 - transformedV * 2.0;
  double nearDepth = 0.0;
  double farDepth = 1.0;
  if (metrics.convention.depthRange ==
      EditorClipDepthRange::negativeOneToOne) {
    nearDepth = -1.0;
  }
  if (metrics.convention.reversedDepth) {
    std::swap(nearDepth, farDepth);
  }
  const auto nearPoint = transformHomogeneous(
      camera.inverseViewProjection, ndcX, ndcY, nearDepth, 1.0);
  const auto farPoint = transformHomogeneous(
      camera.inverseViewProjection, ndcX, ndcY, farDepth, 1.0);
  if (!nearPoint || !farPoint || std::abs(nearPoint->w) <= kMathEpsilon ||
      std::abs(farPoint->w) <= kMathEpsilon) {
    return std::nullopt;
  }
  const OverlayVec3 origin{nearPoint->x / nearPoint->w,
                           nearPoint->y / nearPoint->w,
                           nearPoint->z / nearPoint->w};
  const OverlayVec3 farPosition{farPoint->x / farPoint->w,
                                farPoint->y / farPoint->w,
                                farPoint->z / farPoint->w};
  const auto direction = normalized(subtract(farPosition, origin));
  if (!finiteOverlayVector(origin) || !direction) {
    return std::nullopt;
  }
  return GizmoRay{origin, *direction};
}

std::optional<GizmoRayPlaneHit>
intersectGizmoRayPlane(const GizmoRay &ray, OverlayVec3 planePoint,
                       OverlayVec3 planeNormal,
                       double parallelEpsilon) noexcept {
  const auto direction = normalized(ray.direction);
  const auto normal = normalized(planeNormal);
  if (!finiteOverlayVector(ray.origin) || !finiteOverlayVector(planePoint) ||
      !direction || !normal || !finite(parallelEpsilon) ||
      parallelEpsilon <= 0.0) {
    return std::nullopt;
  }
  const double denominator = dot(*direction, *normal);
  if (!finite(denominator) || std::abs(denominator) < parallelEpsilon) {
    return std::nullopt;
  }
  const double distance =
      dot(subtract(planePoint, ray.origin), *normal) / denominator;
  if (!finite(distance) || distance < 0.0) {
    return std::nullopt;
  }
  const OverlayVec3 position = add(ray.origin, multiply(*direction, distance));
  return finiteOverlayVector(position)
             ? std::optional<GizmoRayPlaneHit>(
                   GizmoRayPlaneHit{distance, position})
             : std::nullopt;
}

GizmoHit classifyGizmoHit(const GizmoRay &localRay,
                          const GizmoGeometryAtlas &geometry,
                          const GizmoHitStyle &hitStyle,
                          GizmoVisibility visibility) noexcept {
  const auto direction = normalized(localRay.direction);
  if (!finiteOverlayVector(localRay.origin) || !direction ||
      !validHitStyle(hitStyle) || !validVisibility(visibility) ||
      !validGizmoStyle(geometry.style()) ||
      geometry.identity().vertexCount == 0 ||
      geometry.identity().indexCount == 0) {
    return {};
  }
  const GizmoRay ray{localRay.origin, *direction};
  GizmoHit best;
  double bestDistance = std::numeric_limits<double>::infinity();

  for (GizmoComponent component : kGizmoComponentOrder) {
    if (!componentVisible(component, visibility)) {
      continue;
    }
    const std::size_t index = gizmoComponentIndex(component);
    const GizmoComponentRangePacket &range = geometry.ranges()[index];
    double expansion = hitStyle.axisPickRadius;
    if (component == GizmoComponent::translateXY ||
        component == GizmoComponent::translateXZ ||
        component == GizmoComponent::translateYZ) {
      expansion = hitStyle.translationPlaneTolerance;
    } else if (component == GizmoComponent::rotateX ||
               component == GizmoComponent::rotateY ||
               component == GizmoComponent::rotateZ) {
      expansion = hitStyle.rotationRingTolerance;
    } else if (component == GizmoComponent::scaleX ||
               component == GizmoComponent::scaleY ||
               component == GizmoComponent::scaleZ) {
      expansion = hitStyle.scaleAxisPickRadius;
    } else if (component == GizmoComponent::uniformScale) {
      expansion = hitStyle.uniformCenterPickHalfExtent;
    }
    if (!rayAabb(ray, range.localBounds, expansion)) {
      continue;
    }

    GizmoHit candidate;
    candidate.component = component;
    if (component == GizmoComponent::translateX ||
        component == GizmoComponent::translateY ||
        component == GizmoComponent::translateZ ||
        component == GizmoComponent::scaleX ||
        component == GizmoComponent::scaleY ||
        component == GizmoComponent::scaleZ) {
      const std::size_t axis = *axisIndex(component);
      const bool scaleComponent =
          component == GizmoComponent::scaleX ||
          component == GizmoComponent::scaleY ||
          component == GizmoComponent::scaleZ;
      const double start = scaleComponent
                               ? geometry.style().scaleCubeSize *
                                     geometry.style().uniformScaleMultiplier * 0.5
                               : 0.0;
      const double end =
          scaleComponent
              ? geometry.style().scaleAxisLength +
                    geometry.style().scaleCubeSize * 0.5
              : geometry.style().axisShaftLength +
                    geometry.style().translationConeHeight;
      const auto distance = closestRaySegment(
          ray, multiply(canonicalAxis(axis), start),
          multiply(canonicalAxis(axis), end));
      const double tolerance = scaleComponent
                                   ? hitStyle.scaleAxisPickRadius
                                   : hitStyle.axisPickRadius;
      if (!distance || distance->distance > tolerance) {
        continue;
      }
      candidate.kind = GizmoHitKind::axisShaftOrTip;
      candidate.rayDistance = distance->rayT;
      candidate.missDistance = distance->distance;
    } else if (component == GizmoComponent::translateXY ||
               component == GizmoComponent::translateXZ ||
               component == GizmoComponent::translateYZ) {
      OverlayVec3 normal{};
      std::array<std::size_t, 2> coordinates{};
      if (component == GizmoComponent::translateXY) {
        normal = {0.0, 0.0, 1.0};
        coordinates = {0u, 1u};
      } else if (component == GizmoComponent::translateXZ) {
        normal = {0.0, 1.0, 0.0};
        coordinates = {0u, 2u};
      } else {
        normal = {1.0, 0.0, 0.0};
        coordinates = {1u, 2u};
      }
      const auto hit = intersectGizmoRayPlane(ray, {}, normal);
      if (!hit) {
        continue;
      }
      const std::array<double, 3> point{hit->position.x, hit->position.y,
                                        hit->position.z};
      const double minimum = geometry.style().translationPlaneOffset;
      const double maximum = minimum + geometry.style().translationPlaneSize;
      const auto intervalDistance = [minimum, maximum](double value) {
        return value < minimum ? minimum - value
                               : (value > maximum ? value - maximum : 0.0);
      };
      const double firstMiss = intervalDistance(point[coordinates[0]]);
      const double secondMiss = intervalDistance(point[coordinates[1]]);
      const double miss = std::hypot(firstMiss, secondMiss);
      if (!finite(miss) || miss > hitStyle.translationPlaneTolerance) {
        continue;
      }
      candidate.kind = GizmoHitKind::translationPlane;
      candidate.rayDistance = hit->distance;
      candidate.missDistance = miss;
    } else if (component == GizmoComponent::rotateX ||
               component == GizmoComponent::rotateY ||
               component == GizmoComponent::rotateZ) {
      const std::size_t axis = *axisIndex(component);
      const auto hit = intersectGizmoRayPlane(ray, {}, canonicalAxis(axis));
      if (!hit) {
        continue;
      }
      const double radial = std::sqrt(dot(hit->position, hit->position));
      const double miss =
          std::abs(radial - geometry.style().rotationRingRadius);
      if (!finite(miss) || miss > hitStyle.rotationRingTolerance) {
        continue;
      }
      candidate.kind = GizmoHitKind::rotationRing;
      candidate.rayDistance = hit->distance;
      candidate.missDistance = miss;
    } else {
      const double half = hitStyle.uniformCenterPickHalfExtent;
      const auto hit = rayAabb(ray, {{-half, -half, -half}, {half, half, half}},
                               0.0);
      if (!hit) {
        continue;
      }
      candidate.kind = GizmoHitKind::uniformCenter;
      candidate.rayDistance = *hit;
      candidate.missDistance = 0.0;
    }

    const double tieTolerance =
        kMathEpsilon * std::max({1.0, bestDistance, candidate.rayDistance});
    if (!best.hit() || candidate.rayDistance < bestDistance - tieTolerance) {
      best = candidate;
      bestDistance = candidate.rayDistance;
    }
  }
  return best;
}

std::optional<GizmoBasis>
deriveGizmoBasis(const GizmoAttachment &attachment,
                 GizmoBasisToken basis) noexcept {
  if (!validGizmoAttachment(attachment) ||
      (basis != GizmoBasisToken::global &&
       basis != GizmoBasisToken::parentLocal)) {
    return std::nullopt;
  }
  const auto inverse = parentLinearInverse(attachment.parentWorld);
  const auto pivot = transformOverlayPoint(attachment.parentWorld,
                                           attachment.local.translation);
  const auto localRotation = normalized(attachment.local.rotation);
  if (!inverse || !pivot || !localRotation) {
    return std::nullopt;
  }

  GizmoBasis result;
  result.token = basis;
  result.pivotWorld = *pivot;
  result.parentDeterminant = inverse->determinant;
  result.parentConditionEstimate = inverse->condition;
  for (std::size_t axis = 0; axis < 3u; ++axis) {
    OverlayVec3 localAxis = canonicalAxis(axis);
    if (basis == GizmoBasisToken::parentLocal) {
      const auto rotated = rotate(*localRotation, localAxis);
      if (!rotated) {
        return std::nullopt;
      }
      localAxis = *rotated;
    }
    const auto worldAxis = normalized(multiplyLinear(attachment.parentWorld,
                                                      localAxis));
    if (!worldAxis) {
      return std::nullopt;
    }
    result.axes[axis] = *worldAxis;
  }
  return result;
}

std::optional<OverlayVec3>
worldToParentLocalDirection(const GizmoAttachment &attachment,
                            OverlayVec3 worldDirection) noexcept {
  const auto inverse = parentLinearInverse(attachment.parentWorld);
  if (!inverse || !finiteOverlayVector(worldDirection)) {
    return std::nullopt;
  }
  const OverlayVec3 result = multiplyLinear(inverse->values, worldDirection);
  return finiteOverlayVector(result) ? std::optional<OverlayVec3>(result)
                                     : std::nullopt;
}

std::optional<OverlayVec3>
parentLocalToWorldDirection(const GizmoAttachment &attachment,
                            OverlayVec3 localDirection) noexcept {
  if (!validGizmoAttachment(attachment) ||
      !finiteOverlayVector(localDirection)) {
    return std::nullopt;
  }
  const OverlayVec3 result = multiplyLinear(attachment.parentWorld,
                                             localDirection);
  return finiteOverlayVector(result) ? std::optional<OverlayVec3>(result)
                                     : std::nullopt;
}

std::optional<GizmoTranslationSolution>
solveGizmoTranslation(GizmoComponent component, const GizmoRay &startRay,
                      const GizmoRay &currentRay,
                      OverlayVec3 viewDirectionWorld,
                      const GizmoAttachment &attachment,
                      GizmoBasisToken basis,
                      std::optional<double> snapStep) noexcept {
  const auto frame = deriveGizmoBasis(attachment, basis);
  const auto viewDirection = normalized(viewDirectionWorld);
  if (!frame || !viewDirection ||
      (snapStep && !snapGizmoScalar(0.0, *snapStep))) {
    return std::nullopt;
  }

  OverlayVec3 worldDelta{};
  if (component == GizmoComponent::translateX ||
      component == GizmoComponent::translateY ||
      component == GizmoComponent::translateZ) {
    const std::size_t axis = *axisIndex(component);
    const OverlayVec3 worldAxis = frame->axes[axis];
    const auto planeNormal =
        normalized(cross(worldAxis, cross(*viewDirection, worldAxis)));
    if (!planeNormal) {
      return std::nullopt;
    }
    const auto start = intersectGizmoRayPlane(startRay, frame->pivotWorld,
                                              *planeNormal);
    const auto current = intersectGizmoRayPlane(currentRay, frame->pivotWorld,
                                                *planeNormal);
    if (!start || !current) {
      return std::nullopt;
    }
    const double distance =
        dot(subtract(current->position, start->position), worldAxis);
    worldDelta = multiply(worldAxis, distance);
  } else if (component == GizmoComponent::translateXY ||
             component == GizmoComponent::translateXZ ||
             component == GizmoComponent::translateYZ) {
    std::size_t firstAxis = 0u;
    std::size_t secondAxis = 1u;
    if (component == GizmoComponent::translateXZ) {
      secondAxis = 2u;
    } else if (component == GizmoComponent::translateYZ) {
      firstAxis = 1u;
      secondAxis = 2u;
    }
    const auto planeNormal =
        normalized(cross(frame->axes[firstAxis], frame->axes[secondAxis]));
    if (!planeNormal) {
      return std::nullopt;
    }
    const auto start = intersectGizmoRayPlane(startRay, frame->pivotWorld,
                                              *planeNormal);
    const auto current = intersectGizmoRayPlane(currentRay, frame->pivotWorld,
                                                *planeNormal);
    if (!start || !current) {
      return std::nullopt;
    }
    worldDelta = subtract(current->position, start->position);
  } else {
    return std::nullopt;
  }

  auto localDelta = worldToParentLocalDirection(attachment, worldDelta);
  if (!localDelta) {
    return std::nullopt;
  }
  OverlayVec3 result = add(attachment.local.translation, *localDelta);
  if (snapStep) {
    const auto snapped = snapGizmoVector(result, *snapStep);
    if (!snapped) {
      return std::nullopt;
    }
    result = *snapped;
    *localDelta = subtract(result, attachment.local.translation);
    const auto snappedWorld =
        parentLocalToWorldDirection(attachment, *localDelta);
    if (!snappedWorld) {
      return std::nullopt;
    }
    worldDelta = *snappedWorld;
  }
  if (!finiteOverlayVector(worldDelta) || !finiteOverlayVector(*localDelta) ||
      !finiteOverlayVector(result)) {
    return std::nullopt;
  }
  return GizmoTranslationSolution{worldDelta, *localDelta, result};
}

std::optional<GizmoRotationSolution>
solveGizmoRotation(GizmoComponent component, const GizmoRay &startRay,
                   const GizmoRay &currentRay,
                   const GizmoAttachment &attachment,
                   GizmoBasisToken basis,
                   std::optional<double> snapDegrees) noexcept {
  if (component != GizmoComponent::rotateX &&
      component != GizmoComponent::rotateY &&
      component != GizmoComponent::rotateZ) {
    return std::nullopt;
  }
  const auto frame = deriveGizmoBasis(attachment, basis);
  if (!frame || (snapDegrees && !snapGizmoScalar(0.0, *snapDegrees))) {
    return std::nullopt;
  }
  const std::size_t axis = *axisIndex(component);
  const OverlayVec3 axisWorld = frame->axes[axis];
  const auto start =
      intersectGizmoRayPlane(startRay, frame->pivotWorld, axisWorld);
  const auto current =
      intersectGizmoRayPlane(currentRay, frame->pivotWorld, axisWorld);
  if (!start || !current) {
    return std::nullopt;
  }
  const auto startDirection =
      normalized(subtract(start->position, frame->pivotWorld));
  const auto currentDirection =
      normalized(subtract(current->position, frame->pivotWorld));
  if (!startDirection || !currentDirection) {
    return std::nullopt;
  }
  const double sine =
      dot(axisWorld, cross(*startDirection, *currentDirection));
  const double cosine = std::clamp(dot(*startDirection, *currentDirection),
                                   -1.0, 1.0);
  double degrees =
      std::atan2(sine, cosine) * 180.0 / std::numbers::pi_v<double>;
  if (!finite(degrees)) {
    return std::nullopt;
  }
  if (snapDegrees) {
    const auto snapped = snapGizmoScalar(degrees, *snapDegrees);
    if (!snapped) {
      return std::nullopt;
    }
    degrees = *snapped;
  }
  const OverlayQuaternion delta =
      axisAngleQuaternion(canonicalAxis(axis),
                          frame->parentDeterminant < 0.0 ? -degrees : degrees);
  const auto startRotation = normalized(attachment.local.rotation);
  if (!startRotation) {
    return std::nullopt;
  }
  const OverlayQuaternion combined =
      basis == GizmoBasisToken::global
          ? multiply(delta, *startRotation)
          : multiply(*startRotation, delta);
  const auto result = normalized(combined);
  if (!result || !finite(delta)) {
    return std::nullopt;
  }
  return GizmoRotationSolution{degrees, axisWorld, delta, *result};
}

std::optional<GizmoScaleSolution>
solveGizmoScale(GizmoComponent component, OverlayVec3 startingScale,
                OverlayVec2 startPointer, OverlayVec2 currentPointer,
                std::optional<double> snapStep, double sensitivity,
                double minimumFactor) noexcept {
  if ((component != GizmoComponent::scaleX &&
       component != GizmoComponent::scaleY &&
       component != GizmoComponent::scaleZ &&
       component != GizmoComponent::uniformScale) ||
      !finiteOverlayVector(startingScale) || !finite(startPointer) ||
      !finite(currentPointer) || !finite(sensitivity) || sensitivity <= 0.0 ||
      !finite(minimumFactor) || minimumFactor <= 0.0 ||
      (snapStep && !snapGizmoScalar(0.0, *snapStep))) {
    return std::nullopt;
  }
  const double screenDelta =
      (currentPointer.x - startPointer.x) -
      (currentPointer.y - startPointer.y);
  const double factor = std::max(minimumFactor, 1.0 + screenDelta * sensitivity);
  if (!finite(screenDelta) || !finite(factor)) {
    return std::nullopt;
  }
  OverlayVec3 result = startingScale;
  const auto apply = [&](double &value) -> bool {
    value *= factor;
    if (!finite(value)) {
      return false;
    }
    if (snapStep) {
      const auto snapped = snapGizmoScalar(value, *snapStep);
      if (!snapped) {
        return false;
      }
      value = *snapped;
    }
    return true;
  };
  bool valid = true;
  if (component == GizmoComponent::scaleX ||
      component == GizmoComponent::uniformScale) {
    valid &= apply(result.x);
  }
  if (component == GizmoComponent::scaleY ||
      component == GizmoComponent::uniformScale) {
    valid &= apply(result.y);
  }
  if (component == GizmoComponent::scaleZ ||
      component == GizmoComponent::uniformScale) {
    valid &= apply(result.z);
  }
  return valid && finiteOverlayVector(result)
             ? std::optional<GizmoScaleSolution>(
                   GizmoScaleSolution{factor, result})
             : std::nullopt;
}

std::optional<double> snapGizmoScalar(double value, double step) noexcept {
  if (!finite(value) || !finite(step) || step <= 0.0) {
    return std::nullopt;
  }
  const double quotient = value / step;
  if (!finite(quotient)) {
    return std::nullopt;
  }
  const double result = std::round(quotient) * step;
  return finite(result) ? std::optional<double>(result) : std::nullopt;
}

std::optional<OverlayVec3> snapGizmoVector(OverlayVec3 value,
                                          double step) noexcept {
  const auto x = snapGizmoScalar(value.x, step);
  const auto y = snapGizmoScalar(value.y, step);
  const auto z = snapGizmoScalar(value.z, step);
  if (!x || !y || !z) {
    return std::nullopt;
  }
  return OverlayVec3{*x, *y, *z};
}

std::optional<GizmoConstantScale>
computeGizmoConstantScale(const EditorViewportMetrics &metrics,
                          const GizmoViewDepthSnapshot &viewDepth,
                          double canonicalVisualDiameter,
                          double nominalLogicalPixels) noexcept {
  if (!finite(canonicalVisualDiameter) || canonicalVisualDiameter <= 0.0 ||
      !finite(nominalLogicalPixels) || nominalLogicalPixels <= 0.0 ||
      viewDepth.generation != generationBinding(metrics) ||
      !finite(viewDepth.positiveViewDepth) ||
      viewDepth.positiveViewDepth <= 0.0) {
    return std::nullopt;
  }
  const auto scale = metrics.constantLogicalPixelScale(nominalLogicalPixels,
                                                        viewDepth.positiveViewDepth);
  if (!scale) {
    return std::nullopt;
  }
  const double worldScale = scale->worldSize / canonicalVisualDiameter;
  if (!finite(worldScale) || worldScale <= 0.0) {
    return std::nullopt;
  }
  return GizmoConstantScale{
      {scale->viewportGeneration, scale->projection, scale->view},
      nominalLogicalPixels, scale->worldUnitsPerLogicalPixel, worldScale};
}

std::optional<GizmoDrawPacket>
buildGizmoDrawPacket(const GizmoGeometryAtlas &geometry,
                     const GizmoAttachment &attachment,
                     const EditorViewportMetrics &metrics,
                     const GizmoViewDepthSnapshot &viewDepth,
                     const GizmoDrawSettings &settings) noexcept {
  if (!settings.visible || !validVisibility(settings.visibility) ||
      !validComponentOrNone(settings.hovered) ||
      !validComponentOrNone(settings.active) ||
      !validGizmoAttachment(attachment) ||
      geometry.identity().vertexCount == 0 ||
      geometry.identity().indexCount == 0) {
    return std::nullopt;
  }
  const auto basis = deriveGizmoBasis(attachment, settings.basis);
  const auto scale = computeGizmoConstantScale(
      metrics, viewDepth, geometry.canonicalVisualDiameter(),
      geometry.style().nominalLogicalPixels);
  if (!basis || !scale) {
    return std::nullopt;
  }

  OverlayMatrix4 model;
  model.elements = {
      basis->axes[0].x * scale->worldScale,
      basis->axes[1].x * scale->worldScale,
      basis->axes[2].x * scale->worldScale,
      basis->pivotWorld.x,
      basis->axes[0].y * scale->worldScale,
      basis->axes[1].y * scale->worldScale,
      basis->axes[2].y * scale->worldScale,
      basis->pivotWorld.y,
      basis->axes[0].z * scale->worldScale,
      basis->axes[1].z * scale->worldScale,
      basis->axes[2].z * scale->worldScale,
      basis->pivotWorld.z,
      0.0, 0.0, 0.0, 1.0};
  if (!finiteOverlayMatrix(model)) {
    return std::nullopt;
  }

  GizmoDrawPacket packet;
  packet.generation = scale->generation;
  packet.basis = settings.basis;
  packet.depth = settings.occlusionDither
                     ? GizmoDepthToken::occlusionDither
                     : GizmoDepthToken::xRay;
  packet.plane = GizmoPlaneToken::checkerDiscard;
  packet.snap = settings.snapEnabled ? GizmoSnapToken::enabled
                                     : GizmoSnapToken::disabled;
  packet.worldScale = scale->worldScale;
  packet.canonicalVisualDiameter = geometry.canonicalVisualDiameter();
  for (GizmoComponent component : kGizmoComponentOrder) {
    GizmoComponentDrawPacket &draw =
        packet.components[gizmoComponentIndex(component)];
    draw.range = geometry.ranges()[gizmoComponentIndex(component)];
    draw.model = model;
    draw.modelToken = modelToken(component);
    draw.color = colorToken(component);
    draw.visible = componentVisible(component, settings.visibility);
    if (component == settings.active) {
      draw.state = GizmoVisualStateToken::active;
    } else if (component == settings.hovered) {
      draw.state = GizmoVisualStateToken::hovered;
    } else {
      draw.state = GizmoVisualStateToken::normal;
    }
  }
  return packet;
}

GizmoInteractionState attachGizmo(const GizmoAttachment &attachment) noexcept {
  if (!validGizmoAttachment(attachment)) {
    return {};
  }
  GizmoInteractionState state;
  state.phase = GizmoInteractionPhase::attachedIdle;
  state.fence = attachmentFence(attachment);
  return state;
}

GizmoActionResolution
beginGizmoDrag(const GizmoInteractionState &state,
               const GizmoAttachment &attachment,
               std::uint64_t transactionId,
               GizmoComponent component) noexcept {
  GizmoActionResolution resolution;
  resolution.state = state;
  if (state.phase != GizmoInteractionPhase::attachedIdle ||
      !matchingFence(state, attachment) || transactionId == 0 ||
      gizmoComponentIndex(component) >= kGizmoComponentCount ||
      transactionId == state.lastTerminalTransactionId) {
    return resolution;
  }
  resolution.state.phase = GizmoInteractionPhase::dragging;
  resolution.state.activeTransactionId = transactionId;
  resolution.state.activeComponent = component;
  resolution.state.retainedPreview = attachment.local;
  resolution.state.hasRetainedPreview = true;
  resolution.state.captureHeld = true;
  GizmoAction action;
  action.kind = GizmoActionKind::beginPreview;
  action.transactionId = transactionId;
  action.nodeId = attachment.nodeId;
  action.documentRevision = attachment.documentRevision;
  action.selectionRevision = attachment.selectionRevision;
  action.component = component;
  action.translation = attachment.local.translation;
  action.rotation = attachment.local.rotation;
  action.scale = attachment.local.scale;
  appendAction(resolution, action);
  return resolution;
}

GizmoActionResolution
updateGizmoPreview(const GizmoInteractionState &state,
                   const GizmoAttachment &attachment,
                   const GizmoLocalTransform &preview) noexcept {
  GizmoActionResolution resolution;
  resolution.state = state;
  const bool active = state.phase == GizmoInteractionPhase::dragging ||
                      state.phase == GizmoInteractionPhase::draggingLatched;
  if (active && (!validGizmoAttachment(attachment) ||
                 state.fence != attachmentFence(attachment))) {
    GizmoAction detach;
    detach.kind = GizmoActionKind::detachRetainingPreview;
    detach.terminal = GizmoTerminalToken::noHistory;
    fillActionIdentity(detach, state);
    appendAction(resolution, detach);
    if (state.captureHeld) {
      GizmoAction releaseCapture;
      releaseCapture.kind = GizmoActionKind::releaseCapture;
      fillActionIdentity(releaseCapture, state);
      appendAction(resolution, releaseCapture);
    }
    resolution.state.phase = GizmoInteractionPhase::detached;
    resolution.state.fence = {};
    resolution.state.lastTerminalTransactionId = state.activeTransactionId;
    resolution.state.activeTransactionId = 0;
    resolution.state.activeComponent = GizmoComponent::none;
    resolution.state.captureHeld = false;
    return resolution;
  }
  const auto previewRotation = normalized(preview.rotation);
  if (state.phase != GizmoInteractionPhase::dragging ||
      state.activeTransactionId == 0 ||
      !finiteOverlayVector(preview.translation) || !previewRotation ||
      !finiteOverlayVector(preview.scale)) {
    return resolution;
  }
  GizmoAction action;
  action.kind = GizmoActionKind::updatePreview;
  fillActionIdentity(action, state);
  action.translation = preview.translation;
  action.rotation = *previewRotation;
  action.scale = preview.scale;
  resolution.state.retainedPreview =
      {action.translation, action.rotation, action.scale};
  resolution.state.hasRetainedPreview = true;
  appendAction(resolution, action);
  return resolution;
}

GizmoActionResolution
resolveGizmoTerminalBatch(const GizmoInteractionState &state,
                          const GizmoTerminalBatch &batch) noexcept {
  GizmoActionResolution resolution;
  resolution.state = state;
  if (!validBatch(batch)) {
    return resolution;
  }
  bool deliveredRelease = false;
  bool escape = false;
  bool focusLost = false;
  for (std::uint8_t index = 0; index < batch.count; ++index) {
    switch (batch.events[index].kind) {
    case GizmoTerminalEventKind::deliveredRelease:
      deliveredRelease = true;
      break;
    case GizmoTerminalEventKind::escapePressed:
      escape = true;
      break;
    case GizmoTerminalEventKind::focusLost:
      focusLost = true;
      break;
    case GizmoTerminalEventKind::suppressedPhysicalMouseUp:
      break;
    }
  }

  const bool active = state.phase == GizmoInteractionPhase::dragging ||
                      state.phase == GizmoInteractionPhase::draggingLatched;
  if (active && escape) {
    GizmoAction clear;
    clear.kind = GizmoActionKind::clearSelection;
    fillActionIdentity(clear, state);
    appendAction(resolution, clear);
    GizmoAction detach;
    detach.kind = GizmoActionKind::detachRetainingPreview;
    detach.terminal = GizmoTerminalToken::noHistory;
    fillActionIdentity(detach, state);
    appendAction(resolution, detach);
    if (state.captureHeld) {
      GizmoAction releaseCapture;
      releaseCapture.kind = GizmoActionKind::releaseCapture;
      fillActionIdentity(releaseCapture, state);
      appendAction(resolution, releaseCapture);
    }
    resolution.state.phase = GizmoInteractionPhase::detached;
    resolution.state.fence = {};
    resolution.state.lastTerminalTransactionId = state.activeTransactionId;
    resolution.state.activeTransactionId = 0;
    resolution.state.activeComponent = GizmoComponent::none;
    resolution.state.captureHeld = false;
    return resolution;
  }

  // A focus boundary wins over a queued physical up.  The input layer should
  // normally classify that up as suppressed, but keeping the resolver
  // defensive prevents a forged/misordered batch from manufacturing history.
  if (state.phase == GizmoInteractionPhase::dragging && focusLost) {
    GizmoAction latch;
    latch.kind = GizmoActionKind::latchRetainingPreview;
    fillActionIdentity(latch, state);
    appendAction(resolution, latch);
    if (state.captureHeld) {
      GizmoAction releaseCapture;
      releaseCapture.kind = GizmoActionKind::releaseCapture;
      fillActionIdentity(releaseCapture, state);
      appendAction(resolution, releaseCapture);
    }
    resolution.state.phase = GizmoInteractionPhase::draggingLatched;
    // Physical capture is gone at the focus boundary.  Keep the logical
    // latched owner/preview for suppression, but do not render a stale active
    // handle or advertise an active component to the next frame.
    resolution.state.activeComponent = GizmoComponent::none;
    resolution.state.captureHeld = false;
    return resolution;
  }

  // Once focus loss has latched the interaction, a later physical-up edge is
  // never eligible to commit, even if an upstream caller mislabels it as
  // delivered.  Only a release in the original dragging phase can finalize
  // the prepared transaction.
  if (state.phase == GizmoInteractionPhase::dragging && deliveredRelease) {
    GizmoAction commit;
    commit.kind = GizmoActionKind::commitPreviewOnce;
    commit.terminal = GizmoTerminalToken::commitHistoryOnce;
    fillActionIdentity(commit, state);
    appendAction(resolution, commit);
    if (state.captureHeld) {
      GizmoAction releaseCapture;
      releaseCapture.kind = GizmoActionKind::releaseCapture;
      fillActionIdentity(releaseCapture, state);
      appendAction(resolution, releaseCapture);
    }
    resolution.state.phase = GizmoInteractionPhase::attachedIdle;
    resolution.state.lastTerminalTransactionId = state.activeTransactionId;
    resolution.state.activeTransactionId = 0;
    resolution.state.activeComponent = GizmoComponent::none;
    resolution.state.captureHeld = false;
    return resolution;
  }


  if (state.phase == GizmoInteractionPhase::attachedIdle && escape) {
    GizmoAction clear;
    clear.kind = GizmoActionKind::clearSelection;
    clear.nodeId = state.fence.nodeId;
    clear.documentRevision = state.fence.documentRevision;
    clear.selectionRevision = state.fence.selectionRevision;
    appendAction(resolution, clear);
    resolution.state.phase = GizmoInteractionPhase::detached;
    resolution.state.fence = {};
  }
  return resolution;
}

} // namespace demo::editor
