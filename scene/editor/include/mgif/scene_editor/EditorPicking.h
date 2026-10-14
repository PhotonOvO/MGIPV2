#pragma once

#include <mgif/scene_editor/EditorClock.h>
#include <mgif/scene_editor/EditorPickingTypes.h>

#include "render/editor/EditorViewportMetrics.h"

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace demo::editor {

struct EditorClickClassifierConfig final {
  EditorTimePoint::duration doubleClickInterval{std::chrono::milliseconds{300}};
  double dragThresholdLogicalPixels{6.0};

  bool operator==(const EditorClickClassifierConfig &) const = default;
};

enum class EditorClickClassifierPhase : std::uint8_t {
  idle = 0,
  singlePending,
  doubleClick,
  drag,
};

struct EditorPickEmission final {
  PickIntent intent{PickIntent::singleSelect};
  LogicalViewportPoint logicalPosition;
  ViewportSample sample;
  std::uint64_t viewportGeneration{0};
  EditorProjectionIdentity projection;
  EditorViewIdentity view;
  EditorTimePoint emittedAt{};

  [[nodiscard]] bool valid() const noexcept;
  bool operator==(const EditorPickEmission &) const = default;
};

// Classifies physical primary-button input only. It owns no request serial,
// selection, camera, renderer, or UI state.
class EditorClickClassifier final {
public:
  explicit EditorClickClassifier(
      const EditorClock &clock,
      EditorClickClassifierConfig config = {}) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] EditorClickClassifierPhase phase() const noexcept;
  [[nodiscard]] bool hasPendingSingle() const noexcept;
  [[nodiscard]] bool primaryDown() const noexcept;

  [[nodiscard]] std::optional<EditorPickEmission>
  primaryPress(LogicalViewportPoint currentPointer,
               const EditorViewportMetrics &currentMetrics,
               bool recognizedDouble) noexcept;
  void pointerMove(LogicalViewportPoint currentPointer) noexcept;
  void primaryRelease(LogicalViewportPoint currentPointer) noexcept;
  [[nodiscard]] std::optional<EditorPickEmission>
  update(LogicalViewportPoint currentPointer,
         const EditorViewportMetrics &currentMetrics,
         bool eligible) noexcept;
  void cancelPhysicalPress() noexcept;
  void reset() noexcept;

private:
  [[nodiscard]] std::optional<EditorPickEmission>
  emission(PickIntent intent, LogicalViewportPoint currentPointer,
           const EditorViewportMetrics &currentMetrics,
           EditorTimePoint now) const noexcept;

  const EditorClock *m_clock{nullptr};
  EditorClickClassifierConfig m_config;
  EditorClickClassifierPhase m_phase{EditorClickClassifierPhase::idle};
  LogicalViewportPoint m_pressPosition;
  EditorTimePoint m_pressTime{};
  bool m_primaryDown{false};
};

[[nodiscard]] std::optional<EditorPickRequest>
makeEditorPickRequest(std::uint64_t serial,
                      const EditorPickEmission &emission,
                      const EditorPickFence &fence,
                      std::uint64_t occurrenceGeneration,
                      const WorldRay &ray) noexcept;

class IEditorPickingOccurrenceLookup {
public:
  virtual ~IEditorPickingOccurrenceLookup() = default;

  [[nodiscard]] virtual std::optional<RenderOccurrenceRecord>
  resolve(RenderOccurrenceId occurrence) const noexcept = 0;
};

enum class EditorSelectionEffectKind : std::uint8_t {
  clear = 0,
  setPrimitive,
  togglePrimitive,
};

struct EditorSelectionEffect final {
  EditorSelectionEffectKind kind{EditorSelectionEffectKind::clear};
  SelectionCause cause{SelectionCause::viewportSingle};
  PrimitiveOccurrenceKey occurrence;
  SelectionRevision expectedRevision{0};
  bool soleHighlightedOccurrenceMatches{false};

  bool operator==(const EditorSelectionEffect &) const = default;
};

struct EditorCameraFocusEffect final {
  EditorWorldPoint center;
  double navigationSpeed{0.0};
  std::uint64_t expectedViewRevision{0};
  std::uint64_t nextViewRevision{0};
  bool keepEye{true};

  [[nodiscard]] bool valid() const noexcept;
  bool operator==(const EditorCameraFocusEffect &) const = default;
};

enum class EditorPickingEffectDisposition : std::uint8_t {
  selection = 0,
  camera,
  blank,
  stale,
  error,
  cancelled,
  invalidTerminal,
};

struct EditorPickingEffect final {
  std::uint64_t serial{0};
  PickIntent intent{PickIntent::singleSelect};
  EditorPickingEffectDisposition disposition{
      EditorPickingEffectDisposition::invalidTerminal};
  std::optional<EditorSelectionEffect> selection;
  std::optional<EditorCameraFocusEffect> camera;
  std::optional<SceneBrowserFocusRequest> browserFocus;

  bool operator==(const EditorPickingEffect &) const = default;
};

struct EditorPickingCommitContext final {
  EditorPickFence currentFence;
  std::uint64_t occurrenceGeneration{0};
  SceneSelectionValue currentSelection;
  SelectionRevision selectionRevision{0};
  bool soleHighlightedOccurrenceMatches{false};
  const IEditorPickingOccurrenceLookup *occurrences{nullptr};

  [[nodiscard]] bool valid() const noexcept;
};

enum class EditorPickingTrackStatus : std::uint8_t {
  accepted = 0,
  rejectedInvalid,
  rejectedSerial,
};

struct EditorPickingTrackResult final {
  EditorPickingTrackStatus status{EditorPickingTrackStatus::rejectedInvalid};
  std::uint64_t serial{0};

  [[nodiscard]] constexpr bool accepted() const noexcept {
    return status == EditorPickingTrackStatus::accepted;
  }

  auto operator<=>(const EditorPickingTrackResult &) const = default;
};

struct EditorPickingControllerStats final {
  std::uint64_t trackedRequests{0};
  std::uint64_t committedTerminals{0};
  std::uint64_t ignoredUnknownTerminals{0};
  std::uint64_t ignoredDuplicateTerminals{0};
  std::uint64_t staleTerminals{0};
  std::uint64_t resetPendingRequests{0};

  bool operator==(const EditorPickingControllerStats &) const = default;
};

// Buffers terminal results in accepted-request order. Selection and camera
// authority remain outside this class; only immutable effects are emitted.
class EditorPickingController final {
public:
  [[nodiscard]] EditorPickingTrackResult
  trackAccepted(const EditorPickRequest &request);
  [[nodiscard]] bool acceptTerminal(const EditorPickResult &result) noexcept;
  [[nodiscard]] std::optional<EditorPickingEffect>
  commitReady(const EditorPickingCommitContext &context) noexcept;
  void resetPending() noexcept;

  [[nodiscard]] std::size_t pendingCount() const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> headSerial() const noexcept;
  [[nodiscard]] const EditorPickingControllerStats &stats() const noexcept;

private:
  struct Pending final {
    EditorPickRequest request;
    std::optional<EditorPickResult> terminal;
  };

  [[nodiscard]] EditorPickingEffect
  commit(const Pending &pending,
         const EditorPickingCommitContext &context) noexcept;
  [[nodiscard]] std::size_t pendingIndex(std::uint64_t serial) const noexcept;

  std::vector<Pending> m_pending;
  std::uint64_t m_lastAcceptedSerial{0};
  std::uint64_t m_lastCommittedSerial{0};
  EditorPickingControllerStats m_stats;
};

} // namespace demo::editor
