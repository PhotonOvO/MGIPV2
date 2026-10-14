#pragma once

#include <mgif/scene_editor/EditorObjectId.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace demo::editor {

// Transient material values consumed by the viewport. Document/history state
// changes only when the draft is committed.
struct SceneMaterialPreview final {
  MaterialId material;
  std::uint32_t materialIndex{std::numeric_limits<std::uint32_t>::max()};
  std::uint64_t documentRevision{0};
  bool baseColorOverridden{false};
  std::array<float, 4> baseColor{1.0f, 1.0f, 1.0f, 1.0f};
  std::optional<float> metallic;
  std::optional<float> roughness;
  std::optional<std::array<float, 3>> emissiveColor;
  std::optional<float> alphaCutoff;
  std::optional<float> normalScale;
  std::optional<float> occlusionStrength;

  [[nodiscard]] bool valid() const noexcept {
    if (!material.valid() ||
        !(baseColorOverridden || metallic || roughness || emissiveColor || alphaCutoff ||
          normalScale || occlusionStrength)) return false;
    if (baseColorOverridden)
      for (float component : baseColor)
        if (!std::isfinite(component) || component < 0.0f || component > 1.0f)
          return false;
    const auto validUnit = [](float value) {
      return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
    };
    if (metallic && !validUnit(*metallic)) return false;
    if (roughness && !validUnit(*roughness)) return false;
    if (emissiveColor)
      for (float component : *emissiveColor)
        if (!std::isfinite(component) || component < 0.0f)
          return false;
    if (alphaCutoff && (!std::isfinite(*alphaCutoff) || *alphaCutoff < 0.0f)) return false;
    if (normalScale && !std::isfinite(*normalScale)) return false;
    if (occlusionStrength && !validUnit(*occlusionStrength)) return false;
    return true;
  }
  bool operator==(const SceneMaterialPreview&) const = default;
};

} // namespace demo::editor

// Kept as a source-compatible name for existing renderer/test integrations.
namespace demo::editor {
using SceneMaterialColorPreview = SceneMaterialPreview;
}
