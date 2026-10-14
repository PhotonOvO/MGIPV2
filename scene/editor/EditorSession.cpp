#include <mgif/scene_editor/EditorSession.h>

#include <memory>
#include <new>
#include <utility>

namespace demo::editor {

EditorSessionBundle::EditorSessionBundle(
    std::unique_ptr<GltfDocument> document,
    std::unique_ptr<SceneEditor> editor) noexcept
    : m_document(std::move(document)), m_editor(std::move(editor)) {}

EditorSessionBundle::~EditorSessionBundle() { retire(); }

SessionPrepareResult
EditorSessionBundle::prepare(std::unique_ptr<GltfDocument> document,
                             EditorLimits limits,
                             EditorFaultInjector *faultInjector) noexcept {
  if (!document)
    return {};
  if (faultInjector != nullptr &&
      faultInjector->shouldFail(EditorFaultPoint::allocation)) {
    return SessionPrepareResult{nullptr,
                                SessionPrepareStatus::allocationFailed};
  }
  try {
    auto editor =
        std::make_unique<SceneEditor>(*document, limits, faultInjector);
    if (faultInjector != nullptr &&
        faultInjector->shouldFail(EditorFaultPoint::allocation)) {
      return SessionPrepareResult{nullptr,
                                  SessionPrepareStatus::allocationFailed};
    }
    return SessionPrepareResult{
        std::unique_ptr<EditorSessionBundle>(
            new EditorSessionBundle(std::move(document), std::move(editor))),
        SessionPrepareStatus::prepared};
  } catch (const std::bad_alloc &) {
    return SessionPrepareResult{nullptr,
                                SessionPrepareStatus::allocationFailed};
  } catch (...) {
    return SessionPrepareResult{nullptr,
                                SessionPrepareStatus::allocationFailed};
  }
}

SessionPrepareResult
EditorSessionBundle::prepare(std::unique_ptr<GltfDocument> document,
                             EditorClock &clock, EditorLimits limits,
                             EditorFaultInjector *faultInjector) noexcept {
  if (!document)
    return {};
  if (faultInjector != nullptr &&
      faultInjector->shouldFail(EditorFaultPoint::allocation)) {
    return SessionPrepareResult{nullptr,
                                SessionPrepareStatus::allocationFailed};
  }
  try {
    auto editor =
        std::make_unique<SceneEditor>(*document, clock, limits, faultInjector);
    if (faultInjector != nullptr &&
        faultInjector->shouldFail(EditorFaultPoint::allocation)) {
      return SessionPrepareResult{nullptr,
                                  SessionPrepareStatus::allocationFailed};
    }
    return SessionPrepareResult{
        std::unique_ptr<EditorSessionBundle>(
            new EditorSessionBundle(std::move(document), std::move(editor))),
        SessionPrepareStatus::prepared};
  } catch (const std::bad_alloc &) {
    return SessionPrepareResult{nullptr,
                                SessionPrepareStatus::allocationFailed};
  } catch (...) {
    return SessionPrepareResult{nullptr,
                                SessionPrepareStatus::allocationFailed};
  }
}

GltfDocument &EditorSessionBundle::document() noexcept { return *m_document; }

const GltfDocument &EditorSessionBundle::document() const noexcept {
  return *m_document;
}

SceneEditor &EditorSessionBundle::editor() noexcept { return *m_editor; }

const SceneEditor &EditorSessionBundle::editor() const noexcept {
  return *m_editor;
}

void EditorSessionBundle::retire() noexcept {
  if (m_retired || !m_editor)
    return;
  m_retired = true;
  m_editor->continuousEdit().abandonForSessionRetirement();
  m_editor->selection().clear();
  m_editor->history().clear();
  m_editor->unbindSelectionAuthority();
}

EditorSessionBundle *ActiveEditorSession::get() noexcept {
  return m_active.get();
}

const EditorSessionBundle *ActiveEditorSession::get() const noexcept {
  return m_active.get();
}

bool ActiveEditorSession::replace(
    std::unique_ptr<EditorSessionBundle> prepared) noexcept {
  if (!prepared)
    return false;
  m_active.swap(prepared);
  if (prepared)
    prepared->retire();
  return true;
}

void ActiveEditorSession::unload() noexcept {
  std::unique_ptr<EditorSessionBundle> previous;
  m_active.swap(previous);
  if (previous)
    previous->retire();
}

} // namespace demo::editor
