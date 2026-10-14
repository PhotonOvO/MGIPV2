#pragma once

#include <mgif/scene_editor/EditorSelectionTypes.h>

#include <cstddef>
#include <vector>

namespace demo::editor {

class GltfDocument;

class ISceneSelectionOccurrenceLookup {
public:
  virtual ~ISceneSelectionOccurrenceLookup() = default;

  [[nodiscard]] virtual EditorDocumentAuthority authority() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t documentRevision() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t sceneActivationEpoch() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t generation() const noexcept = 0;
  [[nodiscard]] virtual bool
  contains(PrimitiveOccurrenceKey occurrence) const noexcept = 0;
  [[nodiscard]] virtual bool
  visible(PrimitiveOccurrenceKey occurrence) const noexcept = 0;
  [[nodiscard]] virtual bool
  nodeHighlightEligible(NodeId node) const noexcept = 0;
};

enum class SceneSelectionSetStatus : std::uint8_t {
  changed = 0,
  unchanged,
  rejectedInvalid,
  rejectedForeignAuthority,
  rejectedStale,
  rejectedHidden,
  rejectedRevisionExhausted,
};

class SceneSelection final {
public:
  explicit SceneSelection(EditorDocumentAuthority authority = {}) noexcept;

  [[nodiscard]] EditorDocumentAuthority authority() const noexcept;
  [[nodiscard]] const SceneSelectionValue &value() const noexcept;
  [[nodiscard]] SelectionRevision revision() const noexcept;
  [[nodiscard]] std::uint64_t documentRevision() const noexcept;
  [[nodiscard]] std::uint64_t sceneActivationEpoch() const noexcept;
  [[nodiscard]] std::uint64_t occurrenceGeneration() const noexcept;
  [[nodiscard]] SceneSelectionSetStatus lastSetStatus() const noexcept;
  [[nodiscard]] std::size_t pendingChangeCount() const noexcept;

  [[nodiscard]] bool set(const GltfDocument &document,
                         SceneSelectionValue value, SelectionCause cause,
                         bool requestBrowserFocus = false);
  [[nodiscard]] bool setPrimitive(
      const GltfDocument &document,
      const ISceneSelectionOccurrenceLookup &occurrences,
      PrimitiveOccurrenceKey occurrence, SelectionCause cause,
      bool requestBrowserFocus = false);
  [[nodiscard]] bool clear(SelectionCause cause);
  [[nodiscard]] bool toggleViewportPrimitive(
      const GltfDocument &document,
      const ISceneSelectionOccurrenceLookup &occurrences,
      PrimitiveOccurrenceKey occurrence,
      bool soleHighlightedOccurrenceMatches);

  [[nodiscard]] bool reconcile(
      const GltfDocument &document, const SelectionRemap *explicitRemap,
      const ISceneSelectionOccurrenceLookup *occurrences = nullptr,
      SelectionCause cause = SelectionCause::documentMutation);

  [[nodiscard]] bool
  highlightEligible(
      const GltfDocument &document,
      const ISceneSelectionOccurrenceLookup *occurrences = nullptr) const
      noexcept;
  [[nodiscard]] static bool
  pickable(const GltfDocument &document,
           const ISceneSelectionOccurrenceLookup &occurrences,
           PrimitiveOccurrenceKey occurrence) noexcept;

  [[nodiscard]] std::vector<SelectionChanged> drainChanges();

private:
  [[nodiscard]] bool apply(SceneSelectionValue value, SelectionCause cause,
                           bool requestBrowserFocus,
                           std::uint64_t documentRevision,
                           std::uint64_t sceneActivationEpoch,
                           std::uint64_t occurrenceGeneration);

  EditorDocumentAuthority m_authority;
  SceneSelectionValue m_value;
  SelectionRevision m_revision{0};
  std::uint64_t m_documentRevision{0};
  std::uint64_t m_sceneActivationEpoch{0};
  std::uint64_t m_occurrenceGeneration{0};
  SceneSelectionSetStatus m_lastStatus{SceneSelectionSetStatus::unchanged};
  std::vector<SelectionChanged> m_pendingChanges;
};

} // namespace demo::editor
