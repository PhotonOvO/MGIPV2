#pragma once

#include <mgif/scene_editor/EditorClock.h>
#include <mgif/scene_editor/EditorFaultInjection.h>
#include <mgif/scene_editor/EditorLimits.h>
#include <mgif/scene_editor/GltfDocument.h>
#include <mgif/scene_editor/SceneEditor.h>

#include <cstdint>
#include <memory>

namespace demo::editor {

enum class SessionPrepareStatus : std::uint8_t {
  prepared = 0,
  noDocument,
  allocationFailed,
};

struct SessionPrepareResult final {
  std::unique_ptr<class EditorSessionBundle> session;
  SessionPrepareStatus status{SessionPrepareStatus::noDocument};
  [[nodiscard]] bool succeeded() const noexcept { return session != nullptr; }
};

class EditorSessionBundle final {
public:
  ~EditorSessionBundle();

  [[nodiscard]] static SessionPrepareResult
  prepare(std::unique_ptr<GltfDocument> document,
          EditorLimits limits = kDefaultEditorLimits,
          EditorFaultInjector *faultInjector = nullptr) noexcept;

  [[nodiscard]] static SessionPrepareResult
  prepare(std::unique_ptr<GltfDocument> document, EditorClock &clock,
          EditorLimits limits = kDefaultEditorLimits,
          EditorFaultInjector *faultInjector = nullptr) noexcept;

  [[nodiscard]] GltfDocument &document() noexcept;
  [[nodiscard]] const GltfDocument &document() const noexcept;
  [[nodiscard]] SceneEditor &editor() noexcept;
  [[nodiscard]] const SceneEditor &editor() const noexcept;

  EditorSessionBundle(const EditorSessionBundle &) = delete;
  EditorSessionBundle &operator=(const EditorSessionBundle &) = delete;
  EditorSessionBundle(EditorSessionBundle &&) = delete;
  EditorSessionBundle &operator=(EditorSessionBundle &&) = delete;

private:
  EditorSessionBundle(std::unique_ptr<GltfDocument> document,
                      std::unique_ptr<SceneEditor> editor) noexcept;
  void retire() noexcept;

  std::unique_ptr<GltfDocument> m_document;
  std::unique_ptr<SceneEditor> m_editor;
  bool m_retired{false};

  friend class ActiveEditorSession;
};

class ActiveEditorSession final {
public:
  [[nodiscard]] EditorSessionBundle *get() noexcept;
  [[nodiscard]] const EditorSessionBundle *get() const noexcept;

  [[nodiscard]] bool
  replace(std::unique_ptr<EditorSessionBundle> prepared) noexcept;
  void unload() noexcept;

private:
  std::unique_ptr<EditorSessionBundle> m_active;
};

} // namespace demo::editor
