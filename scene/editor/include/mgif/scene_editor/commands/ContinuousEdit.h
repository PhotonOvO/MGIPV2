#pragma once

#include <mgif/scene_editor/EditorFaultInjection.h>
#include <mgif/scene_editor/commands/EditorHistory.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace demo::editor {

enum class ContinuousEditKind : std::uint8_t {
  inspectorTransform = 0,
  gizmoTransform,
  material,
  light,
};

enum class ContinuousEditState : std::uint8_t {
  idle = 0,
  previewing,
  suspendedUnfocused,
  committed,
  abandoned,
};

enum class ContinuousStatus : std::uint8_t {
  began = 0,
  previewApplied,
  previewUnchanged,
  committed,
  abandonedRetainingPreview,
  suspended,
  noOp,
  rejected,
};

struct ContinuousFrameSignals final {
  std::uint64_t applicationFrame{0};
  bool inspectorDeactivatedAfterEdit{false};
  bool physicalReleaseDelivered{false};
  bool escapePressed{false};
  bool focusLost{false};
  bool windowFocused{true};
  bool syntheticRelease{false};
  bool selectionDetached{false};
};

struct ContinuousResult final {
  ContinuousStatus status{ContinuousStatus::rejected};
  ContinuousEditState state{ContinuousEditState::idle};
  MutationErrorCode error{MutationErrorCode::none};
  // Present only when preview() applied one adjacent document mutation. This
  // lets the application publish each visual delta without pushing history;
  // finishFrame() still owns the single undo entry for the whole gesture.
  std::optional<CommandResult> previewCommand;
  std::optional<HistoryResult> history;
  std::string message;
};

class ContinuousEditSession final {
public:
  ContinuousEditSession(EditorCommandContext &context, EditorHistory &history,
                        EditorFaultInjector *faultInjector = nullptr) noexcept;

  [[nodiscard]] ContinuousResult beginTransform(ContinuousEditKind kind,
                                                NodeId node);
  [[nodiscard]] ContinuousResult beginMaterial(MaterialId material);
  [[nodiscard]] ContinuousResult beginLight(LightId light);

  [[nodiscard]] ContinuousResult preview(const NodeTransform &value);
  [[nodiscard]] ContinuousResult preview(const MaterialSnapshot &value);
  [[nodiscard]] ContinuousResult preview(const LightSnapshot &value);

  [[nodiscard]] ContinuousResult
  finishFrame(const ContinuousFrameSignals &signals);

  [[nodiscard]] ContinuousEditState state() const noexcept;
  [[nodiscard]] bool active() const noexcept;

private:
  [[nodiscard]] ContinuousResult
  beginPrepared(ContinuousEditKind kind,
                ContinuousCommandPrepareResult prepared);
  [[nodiscard]] ContinuousResult
  previewValue(const ContinuousCommandValue &value);
  [[nodiscard]] ContinuousResult finalizeCommit();
  [[nodiscard]] ContinuousResult currentResult(ContinuousStatus status,
                                               MutationErrorCode error) const;
  [[nodiscard]] bool isGizmo() const noexcept;
  void abandonRetainingPreview(bool clearSelection) noexcept;
  void abandonForSessionRetirement() noexcept;

  EditorCommandContext &m_context;
  EditorHistory &m_history;
  EditorFaultInjector *m_faultInjector{nullptr};
  ContinuousEditState m_state{ContinuousEditState::idle};
  ContinuousEditKind m_kind{ContinuousEditKind::inspectorTransform};
  std::uint64_t m_lastFinalizedFrame{0};
  bool m_hasFinalizedFrame{false};
  std::unique_ptr<ContinuousEditorCommand> m_preparedCommand;

  friend class EditorSessionBundle;
};

} // namespace demo::editor
