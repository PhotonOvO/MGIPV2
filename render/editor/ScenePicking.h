#pragma once

#include <mgif/scene_editor/SceneRenderOccurrenceMapping.h>

#include "../passes/EditorPickingPass.h"

#include <cstddef>
#include <vector>

namespace demo::editor {

struct ScenePickingStats final {
  std::uint64_t hitResults{0};
  std::uint64_t missResults{0};
  std::uint64_t errorResults{0};
  std::uint64_t cancelledResults{0};
  std::uint64_t ignoredUnknownCompletions{0};

  bool operator==(const ScenePickingStats &) const = default;
};

// Required lifetime sink for terminal results produced during destruction.
// Implementations must not allocate or throw, and must outlive ScenePicking.
class IScenePickingTerminalSink {
public:
  virtual ~IScenePickingTerminalSink() = default;
  virtual void onTerminal(const EditorPickResult &result) noexcept = 0;
};

class ScenePicking final {
public:
  ScenePicking(IEditorPickingPassBackend &backend,
               IScenePickingTerminalSink &terminalSink) noexcept;
  ~ScenePicking() noexcept;

  ScenePicking(const ScenePicking &) = delete;
  ScenePicking &operator=(const ScenePicking &) = delete;

  [[nodiscard]] PickSubmitResult
  submit(const EditorPickRequest &request,
         SceneRenderOccurrenceSnapshot snapshot);
  void poll(std::vector<EditorPickResult> &out);
  void cancelAuthority(EditorDocumentAuthority authority);
  void cancelAll();

  [[nodiscard]] std::size_t pendingCount() const noexcept;
  [[nodiscard]] const EditorPickingPassStats &passStats() const noexcept;
  [[nodiscard]] const ScenePickingStats &stats() const noexcept;

private:
  struct PendingSnapshot final {
    std::uint64_t serial{0};
    SceneRenderOccurrenceSnapshot snapshot;
  };

  [[nodiscard]] EditorPickResult resolve(
      const EditorPickingPassCompletion &completion,
      const SceneRenderOccurrenceMapping &snapshot) const noexcept;
  [[nodiscard]] std::size_t pendingIndex(std::uint64_t serial) const noexcept;
  static void shutdownCompletion(
      void *context,
      const EditorPickingPassCompletion &completion) noexcept;
  void deliverShutdownCompletion(
      const EditorPickingPassCompletion &completion) noexcept;

  IEditorPickingPassBackend *m_backend{nullptr};
  IScenePickingTerminalSink *m_terminalSink{nullptr};
  // Declared before m_pass so the pass cancels/retire resources first during
  // reverse-order destruction, while these snapshots are still retained.
  std::vector<PendingSnapshot> m_snapshots;
  EditorPickingPass m_pass;
  std::uint64_t m_lastAcceptedSerial{0};
  ScenePickingStats m_stats;
};

} // namespace demo::editor
