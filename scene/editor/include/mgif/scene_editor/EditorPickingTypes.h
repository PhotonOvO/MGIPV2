#pragma once

#include <mgif/scene_editor/EditorSelectionTypes.h>

#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace demo::editor {

struct RenderOccurrenceId final {
  std::uint32_t slot{0};
  std::uint32_t generation{0};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return generation != 0;
  }

  auto operator<=>(const RenderOccurrenceId &) const = default;
};

// Zero is the background token. Nonzero values only index the immutable
// occurrence snapshot retained by the request that produced the readback.
struct FramebufferPickToken final {
  std::uint32_t value{0};

  [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }

  auto operator<=>(const FramebufferPickToken &) const = default;
};

struct RenderOccurrenceRecord final {
  RenderOccurrenceId id;
  PrimitiveOccurrenceKey key;
  bool visible{false};

  auto operator<=>(const RenderOccurrenceRecord &) const = default;
};

struct EditorSceneFence final {
  EditorDocumentAuthority authority;
  std::uint64_t documentRevision{0};
  std::uint64_t projectionRevision{0};
  SceneId scene;
  std::uint64_t sceneEpoch{0};
  std::uint64_t deviceGeneration{0};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return authority.valid() && scene.valid() &&
           scene.authority() == authority && projectionRevision != 0 &&
           sceneEpoch != 0 && deviceGeneration != 0;
  }

  auto operator<=>(const EditorSceneFence &) const = default;
};

struct EditorPickFence final {
  EditorSceneFence scene;
  std::uint64_t viewportGeneration{0};
  std::uint64_t viewRevision{0};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return scene.valid() && viewportGeneration != 0 && viewRevision != 0;
  }

  auto operator<=>(const EditorPickFence &) const = default;
};

struct EditorWorldPoint final {
  double x{0.0};
  double y{0.0};
  double z{0.0};

  [[nodiscard]] bool finite() const noexcept {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
  }

  auto operator<=>(const EditorWorldPoint &) const = default;
};

struct EditorWorldVector final {
  double x{0.0};
  double y{0.0};
  double z{0.0};

  [[nodiscard]] bool finite() const noexcept {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
  }

  [[nodiscard]] double lengthSquared() const noexcept {
    return x * x + y * y + z * z;
  }

  [[nodiscard]] bool normalized(double tolerance = 1.0e-6) const noexcept {
    const double squared = lengthSquared();
    return finite() && std::isfinite(squared) && tolerance >= 0.0 &&
           std::isfinite(tolerance) && std::abs(squared - 1.0) <= tolerance;
  }

  auto operator<=>(const EditorWorldVector &) const = default;
};

struct WorldRay final {
  EditorWorldPoint origin;
  EditorWorldVector direction;

  [[nodiscard]] bool valid() const noexcept {
    return origin.finite() && direction.normalized();
  }

  auto operator<=>(const WorldRay &) const = default;
};

struct ViewportSample final {
  std::uint32_t pixelX{0};
  std::uint32_t pixelY{0};
  std::uint32_t pixelWidth{0};
  std::uint32_t pixelHeight{0};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return pixelWidth != 0 && pixelHeight != 0 && pixelX < pixelWidth &&
           pixelY < pixelHeight;
  }

  auto operator<=>(const ViewportSample &) const = default;
};

enum class PickIntent : std::uint8_t {
  singleSelect = 0,
  doubleFocus,
};

[[nodiscard]] constexpr bool validPickIntent(PickIntent intent) noexcept {
  return intent == PickIntent::singleSelect ||
         intent == PickIntent::doubleFocus;
}

struct EditorPickRequest final {
  std::uint64_t serial{0};
  PickIntent intent{PickIntent::singleSelect};
  EditorPickFence fence;
  std::uint64_t occurrenceGeneration{0};
  ViewportSample sample;
  WorldRay ray;

  [[nodiscard]] bool valid() const noexcept {
    return serial != 0 && validPickIntent(intent) && fence.valid() &&
           occurrenceGeneration != 0 && sample.valid() && ray.valid();
  }

  auto operator<=>(const EditorPickRequest &) const = default;
};

enum class PickCompletion : std::uint8_t {
  hit = 0,
  miss,
  error,
  cancelled,
};

enum class PickResultReason : std::uint8_t {
  none = 0,
  background,
  invalidRequest,
  fenceMismatch,
  snapshotMismatch,
  invalidToken,
  hiddenOccurrence,
  invalidHit,
  backendRejected,
  backendFailure,
  cancelled,
};

struct EditorPickResult final {
  std::uint64_t serial{0};
  PickIntent intent{PickIntent::singleSelect};
  EditorPickFence fence;
  std::uint64_t occurrenceGeneration{0};
  PickCompletion completion{PickCompletion::error};
  PickResultReason reason{PickResultReason::backendFailure};
  std::optional<RenderOccurrenceId> occurrence;
  std::optional<EditorWorldPoint> worldHit;
  std::optional<double> rayDistance;

  [[nodiscard]] bool terminalPayloadValid() const noexcept {
    if (serial == 0 || !validPickIntent(intent) || !fence.valid() ||
        occurrenceGeneration == 0) {
      return false;
    }
    switch (completion) {
    case PickCompletion::hit:
      return reason == PickResultReason::none && occurrence &&
             occurrence->valid() && worldHit && worldHit->finite() &&
             rayDistance && std::isfinite(*rayDistance) && *rayDistance > 0.0;
    case PickCompletion::miss:
      return reason != PickResultReason::none &&
             reason != PickResultReason::backendFailure &&
             reason != PickResultReason::backendRejected &&
             reason != PickResultReason::cancelled && !occurrence &&
             !worldHit && !rayDistance;
    case PickCompletion::error:
      return (reason == PickResultReason::backendRejected ||
              reason == PickResultReason::backendFailure) &&
             !occurrence && !worldHit && !rayDistance;
    case PickCompletion::cancelled:
      return reason == PickResultReason::cancelled && !occurrence &&
             !worldHit && !rayDistance;
    }
    return false;
  }

  auto operator<=>(const EditorPickResult &) const = default;
};

enum class PickSubmitStatus : std::uint8_t {
  accepted = 0,
  rejectedInvalid,
  rejectedFence,
  rejectedSnapshot,
  rejectedSerial,
  rejectedBackend,
};

struct PickSubmitResult final {
  PickSubmitStatus status{PickSubmitStatus::rejectedInvalid};
  std::uint64_t serial{0};

  [[nodiscard]] constexpr bool accepted() const noexcept {
    return status == PickSubmitStatus::accepted;
  }

  auto operator<=>(const PickSubmitResult &) const = default;
};

struct EditorHighlightState final {
  EditorSceneFence fence;
  SelectionRevision selectionRevision{0};
  std::vector<RenderOccurrenceId> occurrences;

  [[nodiscard]] bool valid() const noexcept {
    if (!fence.valid()) {
      return false;
    }
    for (std::size_t index = 0; index < occurrences.size(); ++index) {
      if (!occurrences[index].valid()) {
        return false;
      }
      for (std::size_t other = index + 1; other < occurrences.size(); ++other) {
        if (occurrences[index] == occurrences[other]) {
          return false;
        }
      }
    }
    return true;
  }

  bool operator==(const EditorHighlightState &) const = default;
};

[[nodiscard]] constexpr bool
sameEditorSceneFence(const EditorSceneFence &left,
                     const EditorSceneFence &right) noexcept {
  return left == right;
}

[[nodiscard]] constexpr bool
sameEditorPickFence(const EditorPickFence &left,
                    const EditorPickFence &right) noexcept {
  return left == right;
}

} // namespace demo::editor
