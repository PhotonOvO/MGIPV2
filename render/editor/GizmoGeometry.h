#pragma once

#include "render/editor/EditorOverlayTypes.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace demo::editor {

inline constexpr std::uint32_t kGizmoGeometryIdentityVersion = 1u;

struct GizmoStyle final {
  double nominalLogicalPixels{50.0};
  double axisShaftRadius{0.02};
  double axisShaftLength{1.0};
  double translationConeRadius{0.06};
  double translationConeHeight{0.25};
  std::uint32_t translationConeSegments{16u};
  double rotationRingRadius{1.3};
  double rotationTubeRadius{0.020};
  std::uint32_t rotationRingSegments{64u};
  std::uint32_t rotationTubeSegments{16u};
  double scaleCubeSize{0.15};
  double scaleAxisLength{1.67};
  double uniformScaleMultiplier{2.0};
  double translationPlaneSize{0.22};
  double translationPlaneOffset{0.5};
  double translationPlaneAlpha{0.45};
  bool occlusionDither{false};

  auto operator<=>(const GizmoStyle &) const = default;
};

struct GizmoVertex final {
  OverlayVec3 position{};
  OverlayVec3 normal{};

  auto operator<=>(const GizmoVertex &) const = default;
};

struct GizmoGeometryIdentity final {
  std::uint32_t version{kGizmoGeometryIdentityVersion};
  std::uint32_t vertexCount{0};
  std::uint32_t indexCount{0};
  std::uint64_t semanticDigest{0};

  auto operator<=>(const GizmoGeometryIdentity &) const = default;
};

class GizmoGeometryAtlas final {
public:
  GizmoGeometryAtlas() = default;

  [[nodiscard]] const GizmoStyle &style() const noexcept { return style_; }
  [[nodiscard]] const std::vector<GizmoVertex> &vertices() const noexcept {
    return vertices_;
  }
  [[nodiscard]] const std::vector<std::uint32_t> &indices() const noexcept {
    return indices_;
  }
  [[nodiscard]] const std::array<GizmoComponentRangePacket,
                                 kGizmoComponentCount> &
  ranges() const noexcept {
    return ranges_;
  }
  [[nodiscard]] const GizmoGeometryIdentity &identity() const noexcept {
    return identity_;
  }
  [[nodiscard]] double canonicalVisualDiameter() const noexcept {
    return canonicalVisualDiameter_;
  }

private:
  friend struct GizmoGeometryBuildAccess;
  friend std::optional<GizmoGeometryAtlas>
  buildGizmoGeometry(const GizmoStyle &) noexcept;

  GizmoStyle style_{};
  std::vector<GizmoVertex> vertices_{};
  std::vector<std::uint32_t> indices_{};
  std::array<GizmoComponentRangePacket, kGizmoComponentCount> ranges_{};
  GizmoGeometryIdentity identity_{};
  double canonicalVisualDiameter_{0.0};
};

[[nodiscard]] bool validGizmoStyle(const GizmoStyle &style) noexcept;

[[nodiscard]] std::optional<GizmoGeometryAtlas>
buildGizmoGeometry(const GizmoStyle &style = {}) noexcept;

// Process-lifetime immutable canonical atlas. Generation failure is converted
// to an empty atlas, but the canonical default style is covered by tests.
[[nodiscard]] const GizmoGeometryAtlas &canonicalGizmoGeometry() noexcept;

} // namespace demo::editor
