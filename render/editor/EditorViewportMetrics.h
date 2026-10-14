#pragma once

#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>

namespace demo::editor {

struct LogicalViewportPoint final {
  double x{0.0};
  double y{0.0};

  auto operator<=>(const LogicalViewportPoint &) const = default;
};

struct LogicalViewportExtent final {
  double width{0.0};
  double height{0.0};

  auto operator<=>(const LogicalViewportExtent &) const = default;
};

struct LogicalViewportRect final {
  LogicalViewportPoint origin;
  LogicalViewportExtent extent;

  auto operator<=>(const LogicalViewportRect &) const = default;
};

struct PhysicalViewportOffset final {
  std::uint32_t x{0};
  std::uint32_t y{0};

  auto operator<=>(const PhysicalViewportOffset &) const = default;
};

struct PhysicalViewportExtent final {
  std::uint32_t width{0};
  std::uint32_t height{0};

  auto operator<=>(const PhysicalViewportExtent &) const = default;
};

struct PhysicalViewportRect final {
  PhysicalViewportOffset origin;
  PhysicalViewportExtent extent;

  auto operator<=>(const PhysicalViewportRect &) const = default;
};

struct EditorFramebufferScale final {
  double x{1.0};
  double y{1.0};

  auto operator<=>(const EditorFramebufferScale &) const = default;
};

enum class EditorViewportSurfaceTransform : std::uint8_t {
  identity = 0,
  rotate90,
  rotate180,
  rotate270,
  mirrorHorizontal,
  mirrorVertical,
  mirrorHorizontalRotate90,
  mirrorHorizontalRotate270,
};

enum class EditorClipDepthRange : std::uint8_t {
  zeroToOne = 0,
  negativeOneToOne,
};

struct EditorProjectionConvention final {
  EditorClipDepthRange depthRange{EditorClipDepthRange::zeroToOne};
  bool reversedDepth{false};
  bool rightHanded{true};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return depthRange == EditorClipDepthRange::zeroToOne ||
           depthRange == EditorClipDepthRange::negativeOneToOne;
  }

  auto operator<=>(const EditorProjectionConvention &) const = default;
};

enum class EditorProjectionKind : std::uint8_t {
  perspective = 0,
  orthographic,
};

struct EditorProjectionIdentity final {
  std::uint64_t revision{0};
  std::uint64_t value{0};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return revision != 0 && value != 0;
  }

  auto operator<=>(const EditorProjectionIdentity &) const = default;
};

struct EditorViewIdentity final {
  std::uint64_t revision{0};
  std::uint64_t value{0};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return revision != 0 && value != 0;
  }

  auto operator<=>(const EditorViewIdentity &) const = default;
};

struct PhysicalViewportPixel final {
  std::uint32_t x{0};
  std::uint32_t y{0};
  std::uint32_t localX{0};
  std::uint32_t localY{0};

  auto operator<=>(const PhysicalViewportPixel &) const = default;
};

struct ConstantLogicalPixelScale final {
  std::uint64_t viewportGeneration{0};
  EditorProjectionIdentity projection;
  EditorViewIdentity view;
  double logicalPixels{0.0};
  double worldUnitsPerLogicalPixel{0.0};
  double worldSize{0.0};

  auto operator<=>(const ConstantLogicalPixelScale &) const = default;
};

// The color viewport is bound to the OS framebuffer DPI transform. A separate
// depth viewport makes unequal target resolutions explicit while preserving
// the same normalized logical coordinates and D4 surface transform.
struct EditorViewportMetrics final {
  LogicalViewportRect logical;
  LogicalViewportPoint framebufferLogicalOrigin;
  PhysicalViewportRect physical;
  PhysicalViewportRect depthPhysical;
  PhysicalViewportExtent colorAttachment;
  PhysicalViewportExtent depthAttachment;
  EditorFramebufferScale framebufferScale;
  EditorViewportSurfaceTransform surfaceTransform{
      EditorViewportSurfaceTransform::identity};
  std::uint64_t viewportGeneration{0};
  EditorProjectionIdentity projection;
  EditorViewIdentity view;
  EditorProjectionConvention convention;
  EditorProjectionKind projectionKind{EditorProjectionKind::perspective};
  double verticalFieldOfViewRadians{0.0};
  double orthographicVerticalSpan{0.0};

  [[nodiscard]] bool valid() const noexcept {
    const auto finite = [](double value) noexcept {
      return std::isfinite(value);
    };
    const double logical_max_x = logical.origin.x + logical.extent.width;
    const double logical_max_y = logical.origin.y + logical.extent.height;
    if (!finite(logical.origin.x) || !finite(logical.origin.y) ||
        !finite(logical.extent.width) || !finite(logical.extent.height) ||
        !finite(framebufferLogicalOrigin.x) ||
        !finite(framebufferLogicalOrigin.y) ||
        logical.extent.width <= 0.0 || logical.extent.height <= 0.0 ||
        !finite(logical_max_x) || !finite(logical_max_y) ||
        logical_max_x <= logical.origin.x ||
        logical_max_y <= logical.origin.y ||
        physical.extent.width == 0 || physical.extent.height == 0 ||
        depthPhysical.extent.width == 0 ||
        depthPhysical.extent.height == 0 ||
        colorAttachment.width == 0 || colorAttachment.height == 0 ||
        depthAttachment.width == 0 || depthAttachment.height == 0 ||
        !finite(framebufferScale.x) || !finite(framebufferScale.y) ||
        framebufferScale.x <= 0.0 || framebufferScale.y <= 0.0 ||
        viewportGeneration == 0 || !projection.valid() || !view.valid() ||
        !convention.valid() || !supportedSurfaceTransform()) {
      return false;
    }

    const std::optional<SourcePixelRect> source = sourcePixelRect();
    const std::optional<PhysicalViewportRect> expected =
        source ? transformedPhysicalRect(*source) : std::nullopt;
    if (!source || !expected || *expected != physical) {
      return false;
    }
    const std::uint64_t depth_end_x =
        static_cast<std::uint64_t>(depthPhysical.origin.x) +
        depthPhysical.extent.width;
    const std::uint64_t depth_end_y =
        static_cast<std::uint64_t>(depthPhysical.origin.y) +
        depthPhysical.extent.height;
    if (depth_end_x > depthAttachment.width ||
        depth_end_y > depthAttachment.height) {
      return false;
    }

    if (projectionKind == EditorProjectionKind::perspective) {
      return finite(verticalFieldOfViewRadians) &&
             verticalFieldOfViewRadians > 0.0 &&
             verticalFieldOfViewRadians < std::numbers::pi_v<double>;
    }
    if (projectionKind == EditorProjectionKind::orthographic) {
      return finite(orthographicVerticalSpan) &&
             orthographicVerticalSpan > 0.0;
    }
    return false;
  }

  [[nodiscard]] bool
  containsLogical(LogicalViewportPoint point) const noexcept {
    if (!valid() || !std::isfinite(point.x) || !std::isfinite(point.y)) {
      return false;
    }
    return point.x >= logical.origin.x && point.y >= logical.origin.y &&
           point.x < logical.origin.x + logical.extent.width &&
           point.y < logical.origin.y + logical.extent.height;
  }

  [[nodiscard]] std::optional<PhysicalViewportPixel>
  logicalToPhysical(LogicalViewportPoint point) const noexcept {
    return logicalToTargetPhysical(point, physical);
  }

  [[nodiscard]] std::optional<PhysicalViewportPixel>
  logicalToDepthPhysical(LogicalViewportPoint point) const noexcept {
    return logicalToTargetPhysical(point, depthPhysical);
  }

  [[nodiscard]] std::optional<ConstantLogicalPixelScale>
  constantLogicalPixelScale(double logicalPixels,
                            double positiveViewDepth) const noexcept {
    if (!valid() || !std::isfinite(logicalPixels) ||
        logicalPixels <= 0.0 || !std::isfinite(positiveViewDepth) ||
        positiveViewDepth <= 0.0) {
      return std::nullopt;
    }

    double units_per_pixel = 0.0;
    if (projectionKind == EditorProjectionKind::perspective) {
      units_per_pixel =
          2.0 * positiveViewDepth *
          std::tan(verticalFieldOfViewRadians * 0.5) /
          logical.extent.height;
    } else {
      units_per_pixel =
          orthographicVerticalSpan / logical.extent.height;
    }
    const double world_size = units_per_pixel * logicalPixels;
    if (!std::isfinite(units_per_pixel) || units_per_pixel <= 0.0 ||
        !std::isfinite(world_size) || world_size <= 0.0) {
      return std::nullopt;
    }
    return ConstantLogicalPixelScale{
        viewportGeneration, projection, view, logicalPixels,
        units_per_pixel, world_size};
  }

private:
  struct SourcePixelRect final {
    std::uint32_t x{0};
    std::uint32_t y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
  };

  [[nodiscard]] bool supportedSurfaceTransform() const noexcept {
    switch (surfaceTransform) {
    case EditorViewportSurfaceTransform::identity:
    case EditorViewportSurfaceTransform::rotate90:
    case EditorViewportSurfaceTransform::rotate180:
    case EditorViewportSurfaceTransform::rotate270:
    case EditorViewportSurfaceTransform::mirrorHorizontal:
    case EditorViewportSurfaceTransform::mirrorVertical:
    case EditorViewportSurfaceTransform::mirrorHorizontalRotate90:
    case EditorViewportSurfaceTransform::mirrorHorizontalRotate270:
      return true;
    }
    return false;
  }

  [[nodiscard]] bool quarterTurn() const noexcept {
    return surfaceTransform == EditorViewportSurfaceTransform::rotate90 ||
           surfaceTransform == EditorViewportSurfaceTransform::rotate270 ||
           surfaceTransform ==
               EditorViewportSurfaceTransform::mirrorHorizontalRotate90 ||
           surfaceTransform ==
               EditorViewportSurfaceTransform::mirrorHorizontalRotate270;
  }

  [[nodiscard]] std::optional<PhysicalViewportPixel>
  logicalToTargetPhysical(LogicalViewportPoint point,
                          PhysicalViewportRect target) const noexcept {
    if (!containsLogical(point)) {
      return std::nullopt;
    }
    const std::uint32_t source_width =
        quarterTurn() ? target.extent.height : target.extent.width;
    const std::uint32_t source_height =
        quarterTurn() ? target.extent.width : target.extent.height;
    const double u = (point.x - logical.origin.x) / logical.extent.width;
    const double v = (point.y - logical.origin.y) / logical.extent.height;
    std::uint32_t source_x = static_cast<std::uint32_t>(
        std::floor(u * static_cast<double>(source_width)));
    std::uint32_t source_y = static_cast<std::uint32_t>(
        std::floor(v * static_cast<double>(source_height)));
    if (source_x >= source_width) {
      source_x = source_width - 1u;
    }
    if (source_y >= source_height) {
      source_y = source_height - 1u;
    }

    std::uint32_t local_x = source_x;
    std::uint32_t local_y = source_y;
    switch (surfaceTransform) {
    case EditorViewportSurfaceTransform::identity:
      break;
    case EditorViewportSurfaceTransform::rotate90:
      local_x = source_height - 1u - source_y;
      local_y = source_x;
      break;
    case EditorViewportSurfaceTransform::rotate180:
      local_x = source_width - 1u - source_x;
      local_y = source_height - 1u - source_y;
      break;
    case EditorViewportSurfaceTransform::rotate270:
      local_x = source_y;
      local_y = source_width - 1u - source_x;
      break;
    case EditorViewportSurfaceTransform::mirrorHorizontal:
      local_x = source_width - 1u - source_x;
      break;
    case EditorViewportSurfaceTransform::mirrorVertical:
      local_y = source_height - 1u - source_y;
      break;
    case EditorViewportSurfaceTransform::mirrorHorizontalRotate90:
      local_x = source_height - 1u - source_y;
      local_y = source_width - 1u - source_x;
      break;
    case EditorViewportSurfaceTransform::mirrorHorizontalRotate270:
      local_x = source_y;
      local_y = source_x;
      break;
    }
    return PhysicalViewportPixel{
        target.origin.x + local_x, target.origin.y + local_y,
        local_x, local_y};
  }

  [[nodiscard]] std::optional<SourcePixelRect>
  sourcePixelRect() const noexcept {
    const double logical_max_x = logical.origin.x + logical.extent.width;
    const double logical_max_y = logical.origin.y + logical.extent.height;
    const double scaled_min_x =
        (logical.origin.x - framebufferLogicalOrigin.x) * framebufferScale.x;
    const double scaled_min_y =
        (logical.origin.y - framebufferLogicalOrigin.y) * framebufferScale.y;
    const double scaled_max_x =
        (logical_max_x - framebufferLogicalOrigin.x) * framebufferScale.x;
    const double scaled_max_y =
        (logical_max_y - framebufferLogicalOrigin.y) * framebufferScale.y;
    if (!std::isfinite(scaled_min_x) || !std::isfinite(scaled_min_y) ||
        !std::isfinite(scaled_max_x) || !std::isfinite(scaled_max_y) ||
        scaled_min_x < 0.0 || scaled_min_y < 0.0 ||
        scaled_max_x <= scaled_min_x || scaled_max_y <= scaled_min_y) {
      return std::nullopt;
    }

    const double start_x = std::floor(scaled_min_x);
    const double start_y = std::floor(scaled_min_y);
    const double end_x = std::ceil(scaled_max_x);
    const double end_y = std::ceil(scaled_max_y);
    const std::uint64_t pre_width =
        quarterTurn() ? colorAttachment.height : colorAttachment.width;
    const std::uint64_t pre_height =
        quarterTurn() ? colorAttachment.width : colorAttachment.height;
    if (end_x > static_cast<double>(pre_width) ||
        end_y > static_cast<double>(pre_height) || end_x <= start_x ||
        end_y <= start_y ||
        end_x > static_cast<double>(
                    std::numeric_limits<std::uint32_t>::max()) ||
        end_y > static_cast<double>(
                    std::numeric_limits<std::uint32_t>::max())) {
      return std::nullopt;
    }
    const auto x = static_cast<std::uint32_t>(start_x);
    const auto y = static_cast<std::uint32_t>(start_y);
    const auto end_pixel_x = static_cast<std::uint32_t>(end_x);
    const auto end_pixel_y = static_cast<std::uint32_t>(end_y);
    return SourcePixelRect{x, y, end_pixel_x - x, end_pixel_y - y};
  }

  [[nodiscard]] std::optional<PhysicalViewportRect>
  transformedPhysicalRect(SourcePixelRect source) const noexcept {
    const std::uint64_t pre_width =
        quarterTurn() ? colorAttachment.height : colorAttachment.width;
    const std::uint64_t pre_height =
        quarterTurn() ? colorAttachment.width : colorAttachment.height;
    std::uint64_t x = source.x;
    std::uint64_t y = source.y;
    std::uint64_t width = source.width;
    std::uint64_t height = source.height;
    switch (surfaceTransform) {
    case EditorViewportSurfaceTransform::identity:
      break;
    case EditorViewportSurfaceTransform::rotate90:
      x = pre_height - (static_cast<std::uint64_t>(source.y) +
                        source.height);
      y = source.x;
      width = source.height;
      height = source.width;
      break;
    case EditorViewportSurfaceTransform::rotate180:
      x = pre_width -
          (static_cast<std::uint64_t>(source.x) + source.width);
      y = pre_height -
          (static_cast<std::uint64_t>(source.y) + source.height);
      break;
    case EditorViewportSurfaceTransform::rotate270:
      x = source.y;
      y = pre_width -
          (static_cast<std::uint64_t>(source.x) + source.width);
      width = source.height;
      height = source.width;
      break;
    case EditorViewportSurfaceTransform::mirrorHorizontal:
      x = pre_width -
          (static_cast<std::uint64_t>(source.x) + source.width);
      break;
    case EditorViewportSurfaceTransform::mirrorVertical:
      y = pre_height -
          (static_cast<std::uint64_t>(source.y) + source.height);
      break;
    case EditorViewportSurfaceTransform::mirrorHorizontalRotate90:
      x = pre_height - (static_cast<std::uint64_t>(source.y) +
                        source.height);
      y = pre_width -
          (static_cast<std::uint64_t>(source.x) + source.width);
      width = source.height;
      height = source.width;
      break;
    case EditorViewportSurfaceTransform::mirrorHorizontalRotate270:
      x = source.y;
      y = source.x;
      width = source.height;
      height = source.width;
      break;
    }
    if (x + width > colorAttachment.width ||
        y + height > colorAttachment.height ||
        x > std::numeric_limits<std::uint32_t>::max() ||
        y > std::numeric_limits<std::uint32_t>::max() ||
        width > std::numeric_limits<std::uint32_t>::max() ||
        height > std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
    return PhysicalViewportRect{
        {static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)},
        {static_cast<std::uint32_t>(width),
         static_cast<std::uint32_t>(height)}};
  }
};

} // namespace demo::editor
