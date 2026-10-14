#pragma once

// CPU-only glTF animation playback contracts.
//
// This header deliberately contains no TinyGLTF (or renderer) types.  A
// document owner adapts its immutable accessors to AnimationAccessorView and
// implements AnimationAccessorAdapter; the evaluator never mutates that
// owner.  Keeping the adapter typed also makes malformed byte-level data fail
// before interpolation can publish a partial pose.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace demo::editor {

class GltfDocument;

enum class AnimationAccessorShape : std::uint8_t {
  invalid = 0,
  scalar,
  vec3,
  vec4,
};

enum class AnimationAccessorComponentType : std::uint8_t {
  invalid = 0,
  float32,
};

enum class AnimationInterpolation : std::uint8_t {
  invalid = 0,
  linear,
  step,
  cubicSpline,

  // glTF spelling aliases are useful at adapter boundaries and preserve a
  // direct correspondence with the source document without exposing it.
  LINEAR = linear,
  STEP = step,
  CUBICSPLINE = cubicSpline,
};

enum class AnimationTargetPath : std::uint8_t {
  invalid = 0,
  translation,
  rotation,
  scale,
};

enum class AnimationTimeMode : std::uint8_t {
  clamp = 0,
  loop,

  Clamp = clamp,
  Loop = loop,
};

// The evaluator accepts only decoded float32 payloads.  The adapter owns the
// backing storage and guarantees that the span remains read-only for the
// duration of a call.
struct AnimationAccessorView final {
  AnimationAccessorShape shape{AnimationAccessorShape::invalid};
  AnimationAccessorComponentType componentType{
      AnimationAccessorComponentType::invalid};
  std::uint64_t count{0};
  std::span<const float> values{};

  [[nodiscard]] constexpr std::size_t componentCount() const noexcept {
    switch (shape) {
    case AnimationAccessorShape::scalar:
      return 1;
    case AnimationAccessorShape::vec3:
      return 3;
    case AnimationAccessorShape::vec4:
      return 4;
    case AnimationAccessorShape::invalid:
      return 0;
    }
    return 0;
  }

  [[nodiscard]] bool valid() const noexcept;
};

struct AnimationSamplerView final {
  std::int32_t inputAccessor{-1};
  std::int32_t outputAccessor{-1};
  AnimationInterpolation interpolation{AnimationInterpolation::invalid};

  bool operator==(const AnimationSamplerView&) const = default;
};

struct AnimationChannelView final {
  std::uint32_t nodeIndex{0};
  std::int32_t samplerIndex{-1};
  AnimationTargetPath path{AnimationTargetPath::invalid};

  bool operator==(const AnimationChannelView&) const = default;
};

struct AnimationClipView final {
  std::span<const AnimationSamplerView> samplers{};
  std::span<const AnimationChannelView> channels{};
  // A zero node count means that the source adapter does not expose a node
  // registry; node indices are still checked for arithmetic validity.
  std::uint64_t nodeCount{0};
};

// Every limit is checked before reserve/index arithmetic.  Defaults are large
// enough for ordinary glTF assets while still preventing an untrusted adapter
// from turning one scrub into an unbounded allocation or scan.
struct AnimationSamplingLimits final {
  std::size_t maxAccessors{1u << 20};
  std::size_t maxSamplers{1u << 20};
  std::size_t maxChannels{1u << 20};
  std::size_t maxKeyframes{1u << 24};
  std::size_t maxValues{1u << 26};
  std::size_t maxNodeCount{1u << 24};

  bool operator==(const AnimationSamplingLimits&) const = default;
};

enum class AnimationSampleStatus : std::uint8_t {
  success = 0,
  invalidAdapter,
  invalidClip,
  invalidAnimationIndex,
  limitExceeded,
  invalidAccessorIndex,
  invalidAccessor,
  invalidAccessorType,
  invalidAccessorCount,
  invalidAccessorPayload,
  invalidInterpolation,
  unsupportedCubicSpline,
  // Alias retained for callers that classify all unsupported interpolation
  // forms together.
  unsupportedInterpolation = unsupportedCubicSpline,
  invalidChannel,
  duplicateChannel,
  invalidTimeRange,
  invalidTime,
  allocationFailure,
};

struct AnimationClipInfo final {
  double startTime{0.0};
  double endTime{0.0};
  std::size_t samplerCount{0};
  std::size_t channelCount{0};
  std::size_t keyframeCount{0};

  [[nodiscard]] bool valid() const noexcept;
};

struct AnimationClipAnalysis final {
  AnimationSampleStatus status{AnimationSampleStatus::invalidClip};
  AnimationClipInfo info{};

  [[nodiscard]] bool succeeded() const noexcept {
    return status == AnimationSampleStatus::success;
  }
  [[nodiscard]] explicit operator bool() const noexcept {
    return succeeded();
  }
};

struct AnimationTransformSample final {
  std::uint32_t nodeIndex{0};
  std::optional<std::array<double, 3>> translation;
  std::optional<std::array<double, 4>> rotation;
  std::optional<std::array<double, 3>> scale;

  bool operator==(const AnimationTransformSample&) const = default;
};

struct AnimationSampleResult final {
  AnimationSampleStatus status{AnimationSampleStatus::invalidClip};
  AnimationClipInfo clip{};
  double normalizedTime{0.0};
  std::vector<AnimationTransformSample> transforms;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == AnimationSampleStatus::success;
  }
  [[nodiscard]] explicit operator bool() const noexcept {
    return succeeded();
  }
};

// A small state carrier for UI/controller owners.  It does not own a
// document, and advancing it never mutates an animation accessor.
struct AnimationPlaybackState final {
  double time{0.0};
  double speed{1.0};
  bool playing{true};
  bool loop{true};

  bool operator==(const AnimationPlaybackState&) const = default;
};

struct AnimationAdvanceResult final {
  AnimationSampleStatus status{AnimationSampleStatus::invalidTime};
  double normalizedTime{0.0};

  [[nodiscard]] bool succeeded() const noexcept {
    return status == AnimationSampleStatus::success;
  }
};

// Typed, read-only adapter boundary.  No method exposes TinyGLTF or mutable
// document storage.  Both accessor() and readAccessor() are intentionally
// available: an owner may override either spelling while the evaluator calls
// readAccessor().  The base implementation fails closed.
class AnimationAccessorAdapter {
public:
  virtual ~AnimationAccessorAdapter() = default;

  [[nodiscard]] virtual std::size_t accessorCount() const noexcept {
    return 0;
  }

  [[nodiscard]] virtual std::optional<AnimationAccessorView> accessor(
      std::size_t) const noexcept {
    return std::nullopt;
  }

  [[nodiscard]] virtual std::optional<AnimationAccessorView> readAccessor(
      std::size_t index) const noexcept {
    return accessor(index);
  }
};

// Convenience adapter for already-decoded typed accessors.  It is useful for
// tests and for document owners that cache immutable accessor payloads.
class SpanAnimationAccessorAdapter final : public AnimationAccessorAdapter {
public:
  explicit SpanAnimationAccessorAdapter(
      std::span<const AnimationAccessorView> accessors) noexcept
      : m_accessors(accessors) {}

  [[nodiscard]] std::size_t accessorCount() const noexcept override {
    return m_accessors.size();
  }

  [[nodiscard]] std::optional<AnimationAccessorView> accessor(
      std::size_t index) const noexcept override;

private:
  std::span<const AnimationAccessorView> m_accessors{};
};

// Read-only snapshot adapter for one authoritative GltfDocument animation.
// The implementation copies only referenced, typed FLOAT accessor payloads;
// TinyGLTF remains private to AnimationPlayback.cpp.
class GltfAnimationAdapter final : public AnimationAccessorAdapter {
public:
  GltfAnimationAdapter(const GltfDocument& document,
                       std::size_t animationIndex,
                       const AnimationSamplingLimits& limits = {}) noexcept;
  ~GltfAnimationAdapter();

  GltfAnimationAdapter(GltfAnimationAdapter&&) noexcept;
  GltfAnimationAdapter& operator=(GltfAnimationAdapter&&) noexcept;

  GltfAnimationAdapter(const GltfAnimationAdapter&) = delete;
  GltfAnimationAdapter& operator=(const GltfAnimationAdapter&) = delete;

  [[nodiscard]] AnimationSampleStatus status() const noexcept;
  [[nodiscard]] bool valid() const noexcept {
    return status() == AnimationSampleStatus::success;
  }
  [[nodiscard]] std::size_t animationIndex() const noexcept;
  [[nodiscard]] AnimationClipView clip() const noexcept;

  [[nodiscard]] std::size_t accessorCount() const noexcept override;
  [[nodiscard]] std::optional<AnimationAccessorView> accessor(
      std::size_t index) const noexcept override;

private:
  struct Impl;
  std::size_t m_animationIndex{std::numeric_limits<std::size_t>::max()};
  AnimationSampleStatus m_status{AnimationSampleStatus::invalidAdapter};
  std::unique_ptr<Impl> m_impl;
};

using GltfDocumentAnimationAdapter = GltfAnimationAdapter;
using GltfAnimationReadOnlyAdapter = GltfAnimationAdapter;

// Deterministic clip validation and range extraction.  A failed analysis has
// no usable range and never returns partially validated metadata.
[[nodiscard]] AnimationClipAnalysis analyzeAnimationClip(
    const AnimationClipView& clip, const AnimationAccessorAdapter& adapter,
    const AnimationSamplingLimits& limits = {}) noexcept;

// Normalize a requested time against a validated range.  Clamp preserves the
// end point; loop maps both integer periods and the exact end point to start.
[[nodiscard]] std::optional<double> normalizeAnimationTime(
    double time, double startTime, double endTime,
    AnimationTimeMode mode = AnimationTimeMode::clamp) noexcept;

// Evaluate all translation/rotation/scale channels atomically.  CUBICSPLINE
// is recognized but deliberately returns unsupportedCubicSpline; no pose is
// published on any failure.
[[nodiscard]] AnimationSampleResult sampleAnimationClip(
    const AnimationClipView& clip, const AnimationAccessorAdapter& adapter,
    double time, AnimationTimeMode mode = AnimationTimeMode::clamp,
    const AnimationSamplingLimits& limits = {}) noexcept;

// Document-facing convenience wrappers.  They construct a bounded typed
// snapshot, then use the same validator/evaluator as the adapter-facing API.
[[nodiscard]] AnimationClipAnalysis analyzeAnimation(
    const GltfDocument& document, std::size_t animationIndex,
    const AnimationSamplingLimits& limits = {}) noexcept;

[[nodiscard]] AnimationSampleResult sampleAnimation(
    const GltfDocument& document, std::size_t animationIndex, double time,
    bool loop = true, const AnimationSamplingLimits& limits = {}) noexcept;

// Advance a playback state using the same normalization rules as sampling.
// Invalid delta/speed/range leaves state unchanged and returns a failure.
[[nodiscard]] AnimationAdvanceResult advanceAnimationPlayback(
    AnimationPlaybackState& state, double deltaSeconds,
    const AnimationClipInfo& clip) noexcept;

// Class form for owners that prefer a named utility over free functions.
class AnimationPlayback final {
public:
  [[nodiscard]] static AnimationClipAnalysis analyze(
      const AnimationClipView& clip, const AnimationAccessorAdapter& adapter,
      const AnimationSamplingLimits& limits = {}) noexcept {
    return analyzeAnimationClip(clip, adapter, limits);
  }

  [[nodiscard]] static AnimationSampleResult sample(
      const AnimationClipView& clip, const AnimationAccessorAdapter& adapter,
      double time, AnimationTimeMode mode = AnimationTimeMode::clamp,
      const AnimationSamplingLimits& limits = {}) noexcept {
    return sampleAnimationClip(clip, adapter, time, mode, limits);
  }

  [[nodiscard]] static AnimationClipAnalysis analyze(
      const GltfDocument& document, std::size_t animationIndex,
      const AnimationSamplingLimits& limits = {}) noexcept {
    return analyzeAnimation(document, animationIndex, limits);
  }

  [[nodiscard]] static AnimationSampleResult sample(
      const GltfDocument& document, std::size_t animationIndex, double time,
      bool loop = true, const AnimationSamplingLimits& limits = {}) noexcept {
    return sampleAnimation(document, animationIndex, time, loop, limits);
  }

  [[nodiscard]] static std::optional<double> normalizeTime(
      double time, double startTime, double endTime,
      AnimationTimeMode mode = AnimationTimeMode::clamp) noexcept {
    return normalizeAnimationTime(time, startTime, endTime, mode);
  }

  [[nodiscard]] static AnimationAdvanceResult advance(
      AnimationPlaybackState& state, double deltaSeconds,
      const AnimationClipInfo& clip) noexcept {
    return advanceAnimationPlayback(state, deltaSeconds, clip);
  }
};

} // namespace demo::editor
