#pragma once

#include "render/editor/EditorOverlayTypes.h"

#include <array>
#include <cstdint>
#include <vector>

namespace demo::editor {

inline constexpr double kGridMinimumBaseUnit = 0.001;
inline constexpr double kGridMaximumBaseUnit = 1000.0;
inline constexpr std::uint32_t kGridDefaultLineCount = 151u;
inline constexpr std::uint32_t kGridMinimumLineCount = 11u;
inline constexpr std::uint32_t kGridMaximumLineCount = 501u;
inline constexpr std::size_t kGridDecadeCount = 4u;
inline constexpr std::size_t kGridActiveLodCount = 3u;

enum class GridModelStatus : std::uint8_t {
  disabled = 0,
  invalidInput,
  ready,
};

struct GridModelSettings final {
  bool enabled{false};
  double baseUnit{1.0};
  std::uint32_t lineCount{kGridDefaultLineCount};
  double distanceFadeMultiplier{2.0};

  auto operator<=>(const GridModelSettings &) const = default;
};

struct GridModelInput final {
  OverlayVec3 cameraPosition{};
  OverlayVec3 cameraForward{0.0, -1.0, 0.0};

  auto operator<=>(const GridModelInput &) const = default;
};

struct GridModelResult final {
  GridModelStatus status{GridModelStatus::invalidInput};
  GridDrawPacket draw{};
  std::vector<GridLinePacket> lines{};
  std::array<GridLinePacket, 2> axes{};
  std::uint8_t axisCount{0};
  std::uint64_t semanticDigest{0};

  [[nodiscard]] bool empty() const noexcept {
    return lines.empty() && axisCount == 0;
  }
};

[[nodiscard]] GridModelResult
buildGridModel(const GridModelSettings &settings,
               const GridModelInput &input,
               const EditorViewportMetrics &metrics) noexcept;

} // namespace demo::editor
