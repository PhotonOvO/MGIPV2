#include "render/editor/GridModel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace demo::editor {
namespace {

constexpr double kDirectionEpsilon = 1.0e-12;
constexpr double kFloorRayEpsilon = 1.0e-6;
constexpr double kMaximumGridLevel = 3.999;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

[[nodiscard]] bool finite(double value) noexcept {
  return std::isfinite(value);
}

[[nodiscard]] bool finite(OverlayVec2 value) noexcept {
  return finite(value.x) && finite(value.y);
}

[[nodiscard]] bool finite(OverlayVec3 value) noexcept {
  return finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] std::optional<OverlayVec3>
normalized(OverlayVec3 value) noexcept {
  if (!finite(value)) {
    return std::nullopt;
  }
  const double largest =
      std::max({std::abs(value.x), std::abs(value.y), std::abs(value.z)});
  if (!finite(largest) || largest <= kDirectionEpsilon) {
    return std::nullopt;
  }
  value.x /= largest;
  value.y /= largest;
  value.z /= largest;
  const double lengthSquared =
      value.x * value.x + value.y * value.y + value.z * value.z;
  if (!finite(lengthSquared) ||
      lengthSquared <= kDirectionEpsilon * kDirectionEpsilon) {
    return std::nullopt;
  }
  const double inverseLength = 1.0 / std::sqrt(lengthSquared);
  value.x *= inverseLength;
  value.y *= inverseLength;
  value.z *= inverseLength;
  return finite(value) ? std::optional<OverlayVec3>(value) : std::nullopt;
}

[[nodiscard]] double saturate(double value) noexcept {
  return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] double smoothstep(double edge0, double edge1,
                                double value) noexcept {
  const double t = saturate((value - edge0) / (edge1 - edge0));
  return t * t * (3.0 - 2.0 * t);
}

[[nodiscard]] std::optional<double> snapped(double value,
                                            double step) noexcept {
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

[[nodiscard]] bool coincidentWithStep(double coordinate,
                                      double step) noexcept {
  const auto nearest = snapped(coordinate, step);
  if (!nearest) {
    return false;
  }
  const double towardPositive =
      std::nextafter(coordinate, std::numeric_limits<double>::infinity());
  const double towardNegative =
      std::nextafter(coordinate, -std::numeric_limits<double>::infinity());
  const double ulp = std::max(std::abs(towardPositive - coordinate),
                              std::abs(coordinate - towardNegative));
  if (!finite(ulp)) {
    return false;
  }
  const double tolerance =
      std::max(std::abs(step) * 1.0e-10, ulp * 2.0);
  return std::abs(coordinate - *nearest) <= tolerance;
}

void hashByte(std::uint64_t &hash, std::uint8_t value) noexcept {
  hash ^= value;
  hash *= kFnvPrime;
}

void hashUint64(std::uint64_t &hash, std::uint64_t value) noexcept {
  // Preserve the canonical little-endian FNV-1a byte stream without paying
  // eight non-inlined hashByte calls in MSVC Debug builds.
  hash ^= static_cast<std::uint8_t>(value);
  hash *= kFnvPrime;
  hash ^= static_cast<std::uint8_t>(value >> 8u);
  hash *= kFnvPrime;
  hash ^= static_cast<std::uint8_t>(value >> 16u);
  hash *= kFnvPrime;
  hash ^= static_cast<std::uint8_t>(value >> 24u);
  hash *= kFnvPrime;
  hash ^= static_cast<std::uint8_t>(value >> 32u);
  hash *= kFnvPrime;
  hash ^= static_cast<std::uint8_t>(value >> 40u);
  hash *= kFnvPrime;
  hash ^= static_cast<std::uint8_t>(value >> 48u);
  hash *= kFnvPrime;
  hash ^= static_cast<std::uint8_t>(value >> 56u);
  hash *= kFnvPrime;
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

void hashLine(std::uint64_t &hash, const GridLinePacket &line) noexcept {
  hashDouble(hash, line.start.x);
  hashDouble(hash, line.start.y);
  hashDouble(hash, line.start.z);
  hashDouble(hash, line.end.x);
  hashDouble(hash, line.end.y);
  hashDouble(hash, line.end.z);
  hashByte(hash, static_cast<std::uint8_t>(line.kind));
  hashByte(hash, static_cast<std::uint8_t>(line.blend));
  hashByte(hash, line.lodIndex);
  hashDouble(hash, line.edgeAlpha);
  hashDouble(hash, line.angleAlpha);
  hashDouble(hash, line.distanceAlpha);
  hashDouble(hash, line.lodAlpha);
  hashDouble(hash, line.alpha);
}

[[nodiscard]] std::uint64_t digest(const GridModelResult &result) noexcept {
  std::uint64_t hash = kFnvOffset;
  hashUint64(hash, 1u);
  hashByte(hash, static_cast<std::uint8_t>(result.status));
  hashUint64(hash, result.draw.generation.viewportGeneration);
  hashUint64(hash, result.draw.generation.projection.revision);
  hashUint64(hash, result.draw.generation.projection.value);
  hashUint64(hash, result.draw.generation.view.revision);
  hashUint64(hash, result.draw.generation.view.value);
  hashDouble(hash, result.draw.baseUnit);
  hashDouble(hash, result.draw.distanceToFloor);
  hashDouble(hash, result.draw.center.x);
  hashDouble(hash, result.draw.center.y);
  for (double step : result.draw.decadeSteps) {
    hashDouble(hash, step);
  }
  for (const GridLodPacket &lod : result.draw.activeLods) {
    hashByte(hash, lod.levelIndex);
    hashDouble(hash, lod.step);
    hashDouble(hash, lod.center.x);
    hashDouble(hash, lod.center.y);
    hashDouble(hash, lod.alpha);
  }
  hashUint64(hash, result.lines.size());
  for (const GridLinePacket &line : result.lines) {
    hashLine(hash, line);
  }
  hashByte(hash, result.axisCount);
  for (std::uint8_t index = 0; index < result.axisCount; ++index) {
    hashLine(hash, result.axes[index]);
  }
  return hash;
}

[[nodiscard]] bool validLine(const GridLinePacket &line) noexcept {
  const auto alpha = [](double value) {
    return finite(value) && value >= 0.0 && value <= 1.0;
  };
  const bool nonDegenerate = line.start != line.end;
  return finite(line.start) && finite(line.end) && nonDegenerate &&
         alpha(line.edgeAlpha) &&
         alpha(line.angleAlpha) && alpha(line.distanceAlpha) &&
         alpha(line.lodAlpha) && alpha(line.alpha);
}

} // namespace

GridModelResult buildGridModel(const GridModelSettings &settings,
                               const GridModelInput &input,
                               const EditorViewportMetrics &metrics) noexcept {
  GridModelResult result;
  if (!settings.enabled) {
    result.status = GridModelStatus::disabled;
    return result;
  }
  if (!metrics.valid() || !finite(settings.baseUnit) ||
      !finite(settings.distanceFadeMultiplier) ||
      settings.distanceFadeMultiplier <= 0.0 ||
      settings.lineCount < kGridMinimumLineCount ||
      settings.lineCount > kGridMaximumLineCount ||
      (settings.lineCount & 1u) == 0u || !finite(input.cameraPosition)) {
    return result;
  }
  const auto forward = normalized(input.cameraForward);
  if (!forward) {
    return result;
  }

  try {
    const double baseUnit = std::clamp(settings.baseUnit,
                                       kGridMinimumBaseUnit,
                                       kGridMaximumBaseUnit);
    const double height = std::abs(input.cameraPosition.y);
    double distance = height;
    OverlayVec2 center{input.cameraPosition.x, input.cameraPosition.z};
    if (metrics.projectionKind == EditorProjectionKind::perspective) {
      const double vertical = std::abs(forward->y);
      const double rayDistance =
          vertical >= kFloorRayEpsilon
              ? std::abs(input.cameraPosition.y / forward->y)
              : height;
      distance = vertical * rayDistance + (1.0 - vertical) * height;
      const double sign = input.cameraPosition.y * forward->y <= 0.0 ? 1.0 : -1.0;
      center.x = input.cameraPosition.x + forward->x * distance * sign;
      center.y = input.cameraPosition.z + forward->z * distance * sign;
    } else if (metrics.projectionKind != EditorProjectionKind::orthographic) {
      return result;
    }
    distance = std::max(kGridMinimumBaseUnit, distance);
    if (!finite(distance) || !finite(center)) {
      return result;
    }

    const double level = std::clamp(
        std::log10(std::max(distance / baseUnit, kGridMinimumBaseUnit)),
        0.0, kMaximumGridLevel);
    if (!finite(level)) {
      return result;
    }
    const double fraction = level - std::floor(level);

    result.draw.generation =
        {metrics.viewportGeneration, metrics.projection, metrics.view};
    result.draw.baseUnit = baseUnit;
    result.draw.distanceToFloor = distance;
    result.draw.center = center;
    result.draw.enabled = true;

    double decade = 1.0;
    std::array<OverlayVec2, kGridDecadeCount> snappedCenters{};
    for (std::size_t index = 0; index < kGridDecadeCount; ++index) {
      const double step = baseUnit * decade;
      const auto centerX = snapped(center.x, step);
      const auto centerZ = snapped(center.y, step);
      if (!finite(step) || !centerX || !centerZ) {
        return GridModelResult{};
      }
      result.draw.decadeSteps[index] = step;
      snappedCenters[index] = {*centerX, *centerZ};
      decade *= 10.0;
    }

    const std::uint8_t levelBase =
        static_cast<std::uint8_t>(std::floor(level));
    const std::uint8_t coarsest =
        static_cast<std::uint8_t>(std::clamp<int>(levelBase + 1, 2, 3));
    const std::array<double, kGridActiveLodCount> lodAlphas{
        0.35 + 0.65 * fraction, 1.0, 1.0 - 0.65 * fraction};
    for (std::size_t drawIndex = 0; drawIndex < kGridActiveLodCount;
         ++drawIndex) {
      const std::uint8_t actual =
          static_cast<std::uint8_t>(coarsest - drawIndex);
      result.draw.activeLods[drawIndex] =
          {actual, result.draw.decadeSteps[actual], snappedCenters[actual],
           saturate(lodAlphas[drawIndex])};
    }

    const std::int64_t halfCount =
        static_cast<std::int64_t>(settings.lineCount / 2u);
    const double coarsestExtent =
        result.draw.activeLods[0].step * static_cast<double>(halfCount);
    if (!finite(coarsestExtent) || coarsestExtent <= 0.0) {
      return GridModelResult{};
    }
    const double angleAlpha =
        smoothstep(0.02, 0.35, std::abs(forward->y));
    const double distanceAlpha = saturate(
        1.0 - distance /
                  (coarsestExtent * settings.distanceFadeMultiplier));
    const std::size_t maximumLines =
        static_cast<std::size_t>(settings.lineCount) *
        2u * kGridActiveLodCount;
    result.lines.reserve(maximumLines);

    for (std::size_t drawIndex = 0; drawIndex < kGridActiveLodCount;
         ++drawIndex) {
      const GridLodPacket &lod = result.draw.activeLods[drawIndex];
      const double lodExtent =
          lod.step * static_cast<double>(halfCount);
      if (!finite(lodExtent) || lodExtent <= 0.0 ||
          !finite(lod.center) || lod.center.x + lod.step == lod.center.x ||
          lod.center.y + lod.step == lod.center.y ||
          !finite(lod.center.x - lodExtent) ||
          !finite(lod.center.x + lodExtent) ||
          !finite(lod.center.y - lodExtent) ||
          !finite(lod.center.y + lodExtent) ||
          lod.center.x - lodExtent == lod.center.x + lodExtent ||
          lod.center.y - lodExtent == lod.center.y + lodExtent) {
        return GridModelResult{};
      }
      const auto duplicateWithCoarser = [&](double coordinate) {
        for (std::size_t coarserIndex = 0; coarserIndex < drawIndex;
             ++coarserIndex) {
          if (coincidentWithStep(
                  coordinate, result.draw.activeLods[coarserIndex].step)) {
            return true;
          }
        }
        return false;
      };
      const auto appendLines = [&](GridLineKind kind) {
        for (std::int64_t offset = -halfCount; offset <= halfCount; ++offset) {
          const double coordinate =
              (kind == GridLineKind::parallelX ? lod.center.y : lod.center.x) +
              static_cast<double>(offset) * lod.step;
          if (coincidentWithStep(coordinate, 0.0) ||
              std::abs(coordinate) <=
                  1.0e-10 * std::max(1.0, std::abs(lod.step)) ||
              duplicateWithCoarser(coordinate)) {
            continue;
          }
          const double normalizedOffset =
              std::abs(static_cast<double>(offset)) /
              static_cast<double>(halfCount);
          const double edgeAlpha =
              1.0 - smoothstep(0.8, 1.0, normalizedOffset);
          GridLinePacket line;
          if (kind == GridLineKind::parallelX) {
            line.start = {lod.center.x - lodExtent, 0.0, coordinate};
            line.end = {lod.center.x + lodExtent, 0.0, coordinate};
          } else {
            line.start = {coordinate, 0.0, lod.center.y - lodExtent};
            line.end = {coordinate, 0.0, lod.center.y + lodExtent};
          }
          line.kind = kind;
          line.blend = GridBlendToken::additive;
          line.lodIndex = lod.levelIndex;
          line.edgeAlpha = saturate(edgeAlpha);
          line.angleAlpha = saturate(angleAlpha);
          line.distanceAlpha = saturate(distanceAlpha);
          line.lodAlpha = saturate(lod.alpha);
          line.alpha = saturate(line.edgeAlpha * line.angleAlpha *
                                line.distanceAlpha * line.lodAlpha);
          if (validLine(line)) {
            result.lines.push_back(line);
          }
        }
      };
      appendLines(GridLineKind::parallelX);
      appendLines(GridLineKind::parallelZ);
    }

    GridLinePacket xAxis;
    xAxis.start = {center.x - coarsestExtent, 0.0, 0.0};
    xAxis.end = {center.x + coarsestExtent, 0.0, 0.0};
    xAxis.kind = GridLineKind::xAxis;
    xAxis.blend = GridBlendToken::alphaOver;
    xAxis.lodIndex = 0xffu;
    xAxis.edgeAlpha = xAxis.angleAlpha = xAxis.distanceAlpha =
        xAxis.lodAlpha = xAxis.alpha = 1.0;
    GridLinePacket zAxis;
    zAxis.start = {0.0, 0.0, center.y - coarsestExtent};
    zAxis.end = {0.0, 0.0, center.y + coarsestExtent};
    zAxis.kind = GridLineKind::zAxis;
    zAxis.blend = GridBlendToken::alphaOver;
    zAxis.lodIndex = 0xffu;
    zAxis.edgeAlpha = zAxis.angleAlpha = zAxis.distanceAlpha =
        zAxis.lodAlpha = zAxis.alpha = 1.0;
    if (!validLine(xAxis) || !validLine(zAxis)) {
      return GridModelResult{};
    }
    result.axes = {xAxis, zAxis};
    result.axisCount = 2u;
    result.draw.generatedLineCount =
        static_cast<std::uint32_t>(result.lines.size() + result.axisCount);
    if (result.lines.size() > maximumLines ||
        result.draw.generatedLineCount > maximumLines + 2u) {
      return GridModelResult{};
    }
    result.status = GridModelStatus::ready;
    result.semanticDigest = digest(result);
    return result;
  } catch (...) {
    return GridModelResult{};
  }
}

} // namespace demo::editor
