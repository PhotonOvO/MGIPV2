#include "render/editor/GizmoGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace demo::editor {

struct GizmoGeometryBuildAccess final {
  [[nodiscard]] static std::vector<GizmoVertex> &
  vertices(GizmoGeometryAtlas &atlas) noexcept {
    return atlas.vertices_;
  }

  [[nodiscard]] static std::vector<std::uint32_t> &
  indices(GizmoGeometryAtlas &atlas) noexcept {
    return atlas.indices_;
  }
};

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::uint32_t kMaximumSegments = 4096u;
constexpr std::size_t kMaximumTorusVertices = 4u * 1024u * 1024u;

[[nodiscard]] bool finite(double value) noexcept {
  return std::isfinite(value);
}

[[nodiscard]] bool finite(OverlayVec3 value) noexcept {
  return finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] OverlayVec3 add(OverlayVec3 left, OverlayVec3 right) noexcept {
  return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] OverlayVec3 multiply(OverlayVec3 value, double scalar) noexcept {
  return {value.x * scalar, value.y * scalar, value.z * scalar};
}

[[nodiscard]] double dot(OverlayVec3 left, OverlayVec3 right) noexcept {
  return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] OverlayVec3 normalized(OverlayVec3 value) noexcept {
  const double length = std::sqrt(dot(value, value));
  return multiply(value, 1.0 / length);
}

struct AxisFrame final {
  OverlayVec3 axis{};
  OverlayVec3 u{};
  OverlayVec3 v{};
};

[[nodiscard]] AxisFrame axisFrame(std::size_t axis) noexcept {
  switch (axis) {
  case 0u:
    return {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
  case 1u:
    return {{0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}};
  default:
    return {{0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}};
  }
}

[[nodiscard]] std::uint32_t vertexIndex(const GizmoGeometryAtlas &atlas) {
  return static_cast<std::uint32_t>(atlas.vertices().size());
}

void appendVertex(GizmoGeometryAtlas &atlas, OverlayVec3 position,
                  OverlayVec3 normal) {
  GizmoGeometryBuildAccess::vertices(atlas).push_back({position, normal});
}

void appendTriangle(GizmoGeometryAtlas &atlas, std::uint32_t first,
                    std::uint32_t second, std::uint32_t third) {
  std::vector<std::uint32_t> &indices =
      GizmoGeometryBuildAccess::indices(atlas);
  indices.push_back(first);
  indices.push_back(second);
  indices.push_back(third);
}

void appendCylinder(GizmoGeometryAtlas &atlas, std::size_t axisIndex,
                    double start, double end, double radius,
                    std::uint32_t segments) {
  const AxisFrame frame = axisFrame(axisIndex);
  const std::uint32_t first = vertexIndex(atlas);
  for (std::uint32_t segment = 0; segment < segments; ++segment) {
    const double angle = 2.0 * std::numbers::pi_v<double> *
                         static_cast<double>(segment) /
                         static_cast<double>(segments);
    const OverlayVec3 radial =
        add(multiply(frame.u, std::cos(angle)),
            multiply(frame.v, std::sin(angle)));
    appendVertex(atlas,
                 add(multiply(frame.axis, start), multiply(radial, radius)),
                 radial);
    appendVertex(atlas,
                 add(multiply(frame.axis, end), multiply(radial, radius)),
                 radial);
  }
  for (std::uint32_t segment = 0; segment < segments; ++segment) {
    const std::uint32_t next = (segment + 1u) % segments;
    const std::uint32_t lower = first + segment * 2u;
    const std::uint32_t upper = lower + 1u;
    const std::uint32_t nextLower = first + next * 2u;
    const std::uint32_t nextUpper = nextLower + 1u;
    appendTriangle(atlas, lower, nextLower, upper);
    appendTriangle(atlas, upper, nextLower, nextUpper);
  }

  const std::uint32_t startCenter = vertexIndex(atlas);
  appendVertex(atlas, multiply(frame.axis, start), multiply(frame.axis, -1.0));
  const std::uint32_t endCenter = vertexIndex(atlas);
  appendVertex(atlas, multiply(frame.axis, end), frame.axis);
  for (std::uint32_t segment = 0; segment < segments; ++segment) {
    const std::uint32_t next = (segment + 1u) % segments;
    const double angle = 2.0 * std::numbers::pi_v<double> *
                         static_cast<double>(segment) /
                         static_cast<double>(segments);
    const double nextAngle = 2.0 * std::numbers::pi_v<double> *
                             static_cast<double>(next) /
                             static_cast<double>(segments);
    const OverlayVec3 radial =
        add(multiply(frame.u, std::cos(angle)),
            multiply(frame.v, std::sin(angle)));
    const OverlayVec3 nextRadial =
        add(multiply(frame.u, std::cos(nextAngle)),
            multiply(frame.v, std::sin(nextAngle)));

    const std::uint32_t startFirst = vertexIndex(atlas);
    appendVertex(atlas,
                 add(multiply(frame.axis, start), multiply(radial, radius)),
                 multiply(frame.axis, -1.0));
    appendVertex(atlas,
                 add(multiply(frame.axis, start),
                     multiply(nextRadial, radius)),
                 multiply(frame.axis, -1.0));
    appendTriangle(atlas, startCenter, startFirst + 1u, startFirst);

    const std::uint32_t endFirst = vertexIndex(atlas);
    appendVertex(atlas,
                 add(multiply(frame.axis, end), multiply(radial, radius)),
                 frame.axis);
    appendVertex(atlas,
                 add(multiply(frame.axis, end), multiply(nextRadial, radius)),
                 frame.axis);
    appendTriangle(atlas, endCenter, endFirst, endFirst + 1u);
  }
}

void appendCone(GizmoGeometryAtlas &atlas, std::size_t axisIndex,
                double base, double height, double radius,
                std::uint32_t segments) {
  const AxisFrame frame = axisFrame(axisIndex);
  const OverlayVec3 tip = multiply(frame.axis, base + height);
  for (std::uint32_t segment = 0; segment < segments; ++segment) {
    const std::uint32_t next = (segment + 1u) % segments;
    const double angle = 2.0 * std::numbers::pi_v<double> *
                         static_cast<double>(segment) /
                         static_cast<double>(segments);
    const double nextAngle = 2.0 * std::numbers::pi_v<double> *
                             static_cast<double>(next) /
                             static_cast<double>(segments);
    const OverlayVec3 radial =
        add(multiply(frame.u, std::cos(angle)),
            multiply(frame.v, std::sin(angle)));
    const OverlayVec3 nextRadial =
        add(multiply(frame.u, std::cos(nextAngle)),
            multiply(frame.v, std::sin(nextAngle)));
    const OverlayVec3 sideNormal =
        normalized(add(multiply(radial, height),
                       multiply(frame.axis, radius)));
    const OverlayVec3 nextSideNormal =
        normalized(add(multiply(nextRadial, height),
                       multiply(frame.axis, radius)));
    const OverlayVec3 tipNormal = normalized(add(sideNormal, nextSideNormal));

    const std::uint32_t side = vertexIndex(atlas);
    appendVertex(atlas,
                 add(multiply(frame.axis, base), multiply(radial, radius)),
                 sideNormal);
    appendVertex(atlas,
                 add(multiply(frame.axis, base),
                     multiply(nextRadial, radius)),
                 nextSideNormal);
    appendVertex(atlas, tip, tipNormal);
    appendTriangle(atlas, side, side + 1u, side + 2u);

    const std::uint32_t cap = vertexIndex(atlas);
    const OverlayVec3 capNormal = multiply(frame.axis, -1.0);
    appendVertex(atlas, multiply(frame.axis, base), capNormal);
    appendVertex(atlas,
                 add(multiply(frame.axis, base),
                     multiply(nextRadial, radius)),
                 capNormal);
    appendVertex(atlas,
                 add(multiply(frame.axis, base), multiply(radial, radius)),
                 capNormal);
    appendTriangle(atlas, cap, cap + 1u, cap + 2u);
  }
}

void appendBox(GizmoGeometryAtlas &atlas, OverlayVec3 minimum,
               OverlayVec3 maximum) {
  struct Face final {
    std::array<OverlayVec3, 4> corners{};
    OverlayVec3 normal{};
  };
  const double x0 = minimum.x;
  const double y0 = minimum.y;
  const double z0 = minimum.z;
  const double x1 = maximum.x;
  const double y1 = maximum.y;
  const double z1 = maximum.z;
  const std::array<Face, 6> faces{
      Face{{OverlayVec3{x1, y0, z0}, OverlayVec3{x1, y1, z0},
            OverlayVec3{x1, y1, z1}, OverlayVec3{x1, y0, z1}},
           {1.0, 0.0, 0.0}},
      Face{{OverlayVec3{x0, y0, z1}, OverlayVec3{x0, y1, z1},
            OverlayVec3{x0, y1, z0}, OverlayVec3{x0, y0, z0}},
           {-1.0, 0.0, 0.0}},
      Face{{OverlayVec3{x0, y1, z0}, OverlayVec3{x0, y1, z1},
            OverlayVec3{x1, y1, z1}, OverlayVec3{x1, y1, z0}},
           {0.0, 1.0, 0.0}},
      Face{{OverlayVec3{x0, y0, z1}, OverlayVec3{x0, y0, z0},
            OverlayVec3{x1, y0, z0}, OverlayVec3{x1, y0, z1}},
           {0.0, -1.0, 0.0}},
      Face{{OverlayVec3{x0, y0, z1}, OverlayVec3{x1, y0, z1},
            OverlayVec3{x1, y1, z1}, OverlayVec3{x0, y1, z1}},
           {0.0, 0.0, 1.0}},
      Face{{OverlayVec3{x1, y0, z0}, OverlayVec3{x0, y0, z0},
            OverlayVec3{x0, y1, z0}, OverlayVec3{x1, y1, z0}},
           {0.0, 0.0, -1.0}}};
  for (const Face &face : faces) {
    const std::uint32_t first = vertexIndex(atlas);
    for (OverlayVec3 corner : face.corners) {
      appendVertex(atlas, corner, face.normal);
    }
    appendTriangle(atlas, first, first + 1u, first + 2u);
    appendTriangle(atlas, first, first + 2u, first + 3u);
  }
}

void appendTorus(GizmoGeometryAtlas &atlas, std::size_t axisIndex,
                 double ringRadius, double tubeRadius,
                 std::uint32_t ringSegments,
                 std::uint32_t tubeSegments) {
  const AxisFrame frame = axisFrame(axisIndex);
  const std::uint32_t first = vertexIndex(atlas);
  for (std::uint32_t ring = 0; ring < ringSegments; ++ring) {
    const double ringAngle = 2.0 * std::numbers::pi_v<double> *
                             static_cast<double>(ring) /
                             static_cast<double>(ringSegments);
    const OverlayVec3 radial =
        add(multiply(frame.u, std::cos(ringAngle)),
            multiply(frame.v, std::sin(ringAngle)));
    for (std::uint32_t tube = 0; tube < tubeSegments; ++tube) {
      const double tubeAngle = 2.0 * std::numbers::pi_v<double> *
                               static_cast<double>(tube) /
                               static_cast<double>(tubeSegments);
      const OverlayVec3 normal =
          add(multiply(radial, std::cos(tubeAngle)),
              multiply(frame.axis, std::sin(tubeAngle)));
      appendVertex(atlas,
                   add(multiply(radial, ringRadius),
                       multiply(normal, tubeRadius)),
                   normal);
    }
  }
  for (std::uint32_t ring = 0; ring < ringSegments; ++ring) {
    const std::uint32_t nextRing = (ring + 1u) % ringSegments;
    for (std::uint32_t tube = 0; tube < tubeSegments; ++tube) {
      const std::uint32_t nextTube = (tube + 1u) % tubeSegments;
      const std::uint32_t firstVertex =
          first + ring * tubeSegments + tube;
      const std::uint32_t secondVertex =
          first + nextRing * tubeSegments + tube;
      const std::uint32_t thirdVertex =
          first + nextRing * tubeSegments + nextTube;
      const std::uint32_t fourthVertex =
          first + ring * tubeSegments + nextTube;
      appendTriangle(atlas, firstVertex, secondVertex, thirdVertex);
      appendTriangle(atlas, firstVertex, thirdVertex, fourthVertex);
    }
  }
}

[[nodiscard]] GizmoComponentRangePacket
finishRange(const GizmoGeometryAtlas &atlas, GizmoComponent component,
            std::uint32_t firstVertex, std::uint32_t firstIndex) {
  GizmoComponentRangePacket range;
  range.component = component;
  range.firstVertex = firstVertex;
  range.vertexCount =
      static_cast<std::uint32_t>(atlas.vertices().size()) - firstVertex;
  range.firstIndex = firstIndex;
  range.indexCount =
      static_cast<std::uint32_t>(atlas.indices().size()) - firstIndex;

  const double infinity = std::numeric_limits<double>::infinity();
  range.localBounds.minimum = {infinity, infinity, infinity};
  range.localBounds.maximum = {-infinity, -infinity, -infinity};
  for (std::uint32_t index = 0; index < range.vertexCount; ++index) {
    const OverlayVec3 position =
        atlas.vertices()[static_cast<std::size_t>(firstVertex + index)].position;
    range.localBounds.minimum.x =
        std::min(range.localBounds.minimum.x, position.x);
    range.localBounds.minimum.y =
        std::min(range.localBounds.minimum.y, position.y);
    range.localBounds.minimum.z =
        std::min(range.localBounds.minimum.z, position.z);
    range.localBounds.maximum.x =
        std::max(range.localBounds.maximum.x, position.x);
    range.localBounds.maximum.y =
        std::max(range.localBounds.maximum.y, position.y);
    range.localBounds.maximum.z =
        std::max(range.localBounds.maximum.z, position.z);
  }
  return range;
}

void hashByte(std::uint64_t &hash, std::uint8_t value) noexcept {
  hash ^= value;
  hash *= kFnvPrime;
}

void hashUint64(std::uint64_t &hash, std::uint64_t value) noexcept {
  for (std::uint32_t byte = 0; byte < 8u; ++byte) {
    hashByte(hash, static_cast<std::uint8_t>((value >> (byte * 8u)) & 0xffu));
  }
}

void hashDouble(std::uint64_t &hash, double value) noexcept {
  if (value == 0.0) {
    hashUint64(hash, 0u);
    hashUint64(hash, 0u);
    return;
  }
  int exponent = 0;
  const double mantissa = std::frexp(value, &exponent);
  constexpr double kSemanticMantissaScale =
      static_cast<double>(1ull << 40u);
  const std::int64_t quantized =
      static_cast<std::int64_t>(std::llround(mantissa *
                                             kSemanticMantissaScale));
  hashUint64(hash, static_cast<std::uint64_t>(quantized));
  hashUint64(hash,
             static_cast<std::uint64_t>(static_cast<std::int64_t>(exponent)));
}

[[nodiscard]] std::uint64_t semanticDigest(
    const GizmoGeometryAtlas &atlas) noexcept {
  std::uint64_t hash = kFnvOffset;
  hashUint64(hash, kGizmoGeometryIdentityVersion);
  const GizmoStyle &style = atlas.style();
  hashDouble(hash, style.nominalLogicalPixels);
  hashDouble(hash, style.axisShaftRadius);
  hashDouble(hash, style.axisShaftLength);
  hashDouble(hash, style.translationConeRadius);
  hashDouble(hash, style.translationConeHeight);
  hashUint64(hash, style.translationConeSegments);
  hashDouble(hash, style.rotationRingRadius);
  hashDouble(hash, style.rotationTubeRadius);
  hashUint64(hash, style.rotationRingSegments);
  hashUint64(hash, style.rotationTubeSegments);
  hashDouble(hash, style.scaleCubeSize);
  hashDouble(hash, style.scaleAxisLength);
  hashDouble(hash, style.uniformScaleMultiplier);
  hashDouble(hash, style.translationPlaneSize);
  hashDouble(hash, style.translationPlaneOffset);
  hashDouble(hash, style.translationPlaneAlpha);
  hashByte(hash, style.occlusionDither ? 1u : 0u);
  for (const GizmoComponentRangePacket &range : atlas.ranges()) {
    hashByte(hash, static_cast<std::uint8_t>(range.component));
    hashUint64(hash, range.firstVertex);
    hashUint64(hash, range.vertexCount);
    hashUint64(hash, range.firstIndex);
    hashUint64(hash, range.indexCount);
  }
  for (std::uint32_t index : atlas.indices()) {
    hashUint64(hash, index);
  }
  return hash;
}

} // namespace

bool validGizmoStyle(const GizmoStyle &style) noexcept {
  const bool finiteValues =
      finite(style.nominalLogicalPixels) && finite(style.axisShaftRadius) &&
      finite(style.axisShaftLength) &&
      finite(style.translationConeRadius) &&
      finite(style.translationConeHeight) &&
      finite(style.rotationRingRadius) && finite(style.rotationTubeRadius) &&
      finite(style.scaleCubeSize) && finite(style.scaleAxisLength) &&
      finite(style.uniformScaleMultiplier) &&
      finite(style.translationPlaneSize) &&
      finite(style.translationPlaneOffset) &&
      finite(style.translationPlaneAlpha);
  const std::uint64_t torusVertices =
      static_cast<std::uint64_t>(style.rotationRingSegments) *
      static_cast<std::uint64_t>(style.rotationTubeSegments);
  return finiteValues && style.nominalLogicalPixels > 0.0 &&
         style.axisShaftRadius > 0.0 && style.axisShaftLength > 0.0 &&
         style.translationConeRadius > 0.0 &&
         style.translationConeHeight > 0.0 &&
         style.translationConeSegments >= 3u &&
         style.translationConeSegments <= kMaximumSegments &&
         style.rotationRingRadius > style.rotationTubeRadius &&
         style.rotationTubeRadius > 0.0 &&
         style.rotationRingSegments >= 3u &&
         style.rotationRingSegments <= kMaximumSegments &&
         style.rotationTubeSegments >= 3u &&
         style.rotationTubeSegments <= kMaximumSegments &&
         torusVertices <= kMaximumTorusVertices && style.scaleCubeSize > 0.0 &&
         style.scaleAxisLength > style.scaleCubeSize &&
         style.uniformScaleMultiplier > 0.0 &&
         style.translationPlaneSize > 0.0 &&
         style.translationPlaneOffset >= 0.0 &&
         style.translationPlaneAlpha >= 0.0 &&
         style.translationPlaneAlpha <= 1.0;
}

std::optional<GizmoGeometryAtlas>
buildGizmoGeometry(const GizmoStyle &style) noexcept {
  if (!validGizmoStyle(style)) {
    return std::nullopt;
  }

  try {
    GizmoGeometryAtlas atlas;
    atlas.style_ = style;
    const std::uint64_t estimatedTorusVertices =
        3ull * style.rotationRingSegments * style.rotationTubeSegments;
    atlas.vertices_.reserve(static_cast<std::size_t>(estimatedTorusVertices) +
                            2048u);
    atlas.indices_.reserve(
        static_cast<std::size_t>(estimatedTorusVertices * 6ull) + 4096u);

    for (GizmoComponent component : kGizmoComponentOrder) {
      const std::uint32_t firstVertex =
          static_cast<std::uint32_t>(atlas.vertices_.size());
      const std::uint32_t firstIndex =
          static_cast<std::uint32_t>(atlas.indices_.size());
      const std::size_t componentIndex = gizmoComponentIndex(component);

      if (componentIndex <= gizmoComponentIndex(GizmoComponent::translateZ)) {
        appendCylinder(atlas, componentIndex, 0.0, style.axisShaftLength,
                       style.axisShaftRadius,
                       style.translationConeSegments);
        appendCone(atlas, componentIndex, style.axisShaftLength,
                   style.translationConeHeight, style.translationConeRadius,
                   style.translationConeSegments);
      } else if (component == GizmoComponent::translateXY) {
        const double thickness = style.axisShaftRadius * 0.25;
        appendBox(atlas,
                  {style.translationPlaneOffset, style.translationPlaneOffset,
                   -thickness},
                  {style.translationPlaneOffset + style.translationPlaneSize,
                   style.translationPlaneOffset + style.translationPlaneSize,
                   thickness});
      } else if (component == GizmoComponent::translateXZ) {
        const double thickness = style.axisShaftRadius * 0.25;
        appendBox(atlas,
                  {style.translationPlaneOffset, -thickness,
                   style.translationPlaneOffset},
                  {style.translationPlaneOffset + style.translationPlaneSize,
                   thickness,
                   style.translationPlaneOffset + style.translationPlaneSize});
      } else if (component == GizmoComponent::translateYZ) {
        const double thickness = style.axisShaftRadius * 0.25;
        appendBox(atlas,
                  {-thickness, style.translationPlaneOffset,
                   style.translationPlaneOffset},
                  {thickness,
                   style.translationPlaneOffset + style.translationPlaneSize,
                   style.translationPlaneOffset + style.translationPlaneSize});
      } else if (componentIndex >= gizmoComponentIndex(GizmoComponent::rotateX) &&
                 componentIndex <= gizmoComponentIndex(GizmoComponent::rotateZ)) {
        appendTorus(atlas,
                    componentIndex - gizmoComponentIndex(GizmoComponent::rotateX),
                    style.rotationRingRadius, style.rotationTubeRadius,
                    style.rotationRingSegments, style.rotationTubeSegments);
      } else if (componentIndex >= gizmoComponentIndex(GizmoComponent::scaleX) &&
                 componentIndex <= gizmoComponentIndex(GizmoComponent::scaleZ)) {
        const std::size_t axis =
            componentIndex - gizmoComponentIndex(GizmoComponent::scaleX);
        appendCylinder(atlas, axis,
                       style.scaleCubeSize * style.uniformScaleMultiplier * 0.5,
                       style.scaleAxisLength, style.axisShaftRadius,
                       style.translationConeSegments);
        const AxisFrame frame = axisFrame(axis);
        const OverlayVec3 center = multiply(frame.axis, style.scaleAxisLength);
        const OverlayVec3 half{style.scaleCubeSize * 0.5,
                               style.scaleCubeSize * 0.5,
                               style.scaleCubeSize * 0.5};
        appendBox(atlas,
                  {center.x - half.x, center.y - half.y, center.z - half.z},
                  {center.x + half.x, center.y + half.y, center.z + half.z});
      } else {
        const double half = style.scaleCubeSize *
                            style.uniformScaleMultiplier * 0.5;
        appendBox(atlas, {-half, -half, -half}, {half, half, half});
      }

      atlas.ranges_[componentIndex] =
          finishRange(atlas, component, firstVertex, firstIndex);
    }

    OverlayVec3 overallMinimum{
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity()};
    OverlayVec3 overallMaximum{-overallMinimum.x, -overallMinimum.y,
                               -overallMinimum.z};
    for (const GizmoComponentRangePacket &range : atlas.ranges_) {
      overallMinimum.x = std::min(overallMinimum.x, range.localBounds.minimum.x);
      overallMinimum.y = std::min(overallMinimum.y, range.localBounds.minimum.y);
      overallMinimum.z = std::min(overallMinimum.z, range.localBounds.minimum.z);
      overallMaximum.x = std::max(overallMaximum.x, range.localBounds.maximum.x);
      overallMaximum.y = std::max(overallMaximum.y, range.localBounds.maximum.y);
      overallMaximum.z = std::max(overallMaximum.z, range.localBounds.maximum.z);
    }
    atlas.canonicalVisualDiameter_ =
        std::max({overallMaximum.x - overallMinimum.x,
                  overallMaximum.y - overallMinimum.y,
                  overallMaximum.z - overallMinimum.z});
    if (!finite(atlas.canonicalVisualDiameter_) ||
        atlas.canonicalVisualDiameter_ <= 0.0 ||
        atlas.vertices_.size() > std::numeric_limits<std::uint32_t>::max() ||
        atlas.indices_.size() > std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
    for (const GizmoVertex &vertex : atlas.vertices_) {
      const double normalLength = std::sqrt(dot(vertex.normal, vertex.normal));
      if (!finite(vertex.position) || !finite(vertex.normal) ||
          !finite(normalLength) || normalLength <= 0.0) {
        return std::nullopt;
      }
    }
    for (std::uint32_t index : atlas.indices_) {
      if (index >= atlas.vertices_.size()) {
        return std::nullopt;
      }
    }

    atlas.identity_.vertexCount =
        static_cast<std::uint32_t>(atlas.vertices_.size());
    atlas.identity_.indexCount =
        static_cast<std::uint32_t>(atlas.indices_.size());
    atlas.identity_.semanticDigest = semanticDigest(atlas);
    return atlas;
  } catch (...) {
    return std::nullopt;
  }
}

const GizmoGeometryAtlas &canonicalGizmoGeometry() noexcept {
  static const GizmoGeometryAtlas atlas = [] {
    std::optional<GizmoGeometryAtlas> generated = buildGizmoGeometry();
    return generated ? std::move(*generated) : GizmoGeometryAtlas{};
  }();
  return atlas;
}

} // namespace demo::editor
