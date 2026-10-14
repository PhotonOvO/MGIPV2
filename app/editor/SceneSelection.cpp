#include "SceneSelection.h"

#include <mgif/scene_editor/GltfDocument.h>

#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace demo::editor {
namespace {

template <typename Id>
[[nodiscard]] bool validForAuthority(
    Id id, EditorDocumentAuthority authority) noexcept {
  return id.valid() && authority.valid() && id.authority() == authority;
}

[[nodiscard]] bool validOccurrence(
    PrimitiveOccurrenceKey occurrence,
    EditorDocumentAuthority authority) noexcept {
  return validForAuthority(occurrence.scene, authority) &&
         validForAuthority(occurrence.node, authority) &&
         validForAuthority(occurrence.primitive, authority);
}

[[nodiscard]] bool structurallyValid(
    const SceneSelectionValue &value,
    EditorDocumentAuthority authority) noexcept {
  return std::visit(
      [authority](const auto &selected) noexcept {
        using Selected = std::decay_t<decltype(selected)>;
        if constexpr (std::is_same_v<Selected, std::monostate>) {
          return true;
        } else if constexpr (std::is_same_v<Selected,
                                            PrimitiveOccurrenceKey>) {
          return validOccurrence(selected, authority);
        } else {
          return validForAuthority(selected, authority);
        }
      },
      value);
}

[[nodiscard]] bool currentLookup(
    const GltfDocument &document,
    const ISceneSelectionOccurrenceLookup &occurrences) noexcept {
  return occurrences.authority() == document.documentAuthority() &&
         occurrences.documentRevision() == document.revision() &&
         occurrences.sceneActivationEpoch() ==
             document.sceneActivationEpoch() &&
         occurrences.generation() != 0;
}

[[nodiscard]] std::optional<MeshId>
occurrenceMesh(const GltfDocument &document,
               PrimitiveId primitive) noexcept {
  const std::optional<PrimitiveAddress> address =
      document.primitiveAddress(primitive);
  if (!address) {
    return std::nullopt;
  }
  return MeshId::from(
      document.idAt(EditorObjectKind::mesh, address->meshIndex));
}

[[nodiscard]] bool liveOccurrence(
    const GltfDocument &document,
    const ISceneSelectionOccurrenceLookup &occurrences,
    PrimitiveOccurrenceKey occurrence) noexcept {
  const EditorDocumentAuthority authority = document.documentAuthority();
  if (!currentLookup(document, occurrences) ||
      !validOccurrence(occurrence, authority) ||
      !document.contains(occurrence.scene.untyped()) ||
      !document.contains(occurrence.node.untyped()) ||
      !document.contains(occurrence.primitive.untyped())) {
    return false;
  }
  const std::optional<MeshId> mesh =
      occurrenceMesh(document, occurrence.primitive);
  return mesh && document.ownsPrimitiveOccurrence(
                     occurrence.scene, occurrence.node, *mesh,
                     occurrence.primitive) &&
         occurrences.contains(occurrence);
}

[[nodiscard]] bool liveSelection(
    const GltfDocument &document, const SceneSelectionValue &value,
    const ISceneSelectionOccurrenceLookup *occurrences) noexcept {
  const EditorDocumentAuthority authority = document.documentAuthority();
  if (!structurallyValid(value, authority)) {
    return false;
  }
  return std::visit(
      [&document, occurrences](const auto &selected) noexcept {
        using Selected = std::decay_t<decltype(selected)>;
        if constexpr (std::is_same_v<Selected, std::monostate>) {
          return true;
        } else if constexpr (std::is_same_v<Selected,
                                            PrimitiveOccurrenceKey>) {
          return occurrences &&
                 liveOccurrence(document, *occurrences, selected);
        } else {
          return document.contains(selected.untyped());
        }
      },
      value);
}

enum class RemapStatus : std::uint8_t {
  notMapped = 0,
  mapped,
  invalid,
};

struct RemapResult final {
  RemapStatus status{RemapStatus::notMapped};
  SceneSelectionValue value{};
};

template <typename Id>
[[nodiscard]] RemapResult remapTypedId(const SelectionRemap &remap,
                                       Id source) noexcept {
  std::optional<Id> result;
  bool matched = false;
  for (const SelectionIdRemap &entry : remap.ids) {
    if (entry.before != source.untyped()) {
      continue;
    }
    if (matched) {
      return {RemapStatus::invalid, {}};
    }
    matched = true;
    result = Id::from(entry.after);
    if (!result) {
      return {RemapStatus::invalid, {}};
    }
  }
  if (!matched) {
    return {};
  }
  return remap.complete ? RemapResult{RemapStatus::mapped, *result}
                        : RemapResult{RemapStatus::invalid, {}};
}

[[nodiscard]] RemapResult
remapSelection(const SelectionRemap &remap,
               const SceneSelectionValue &value) noexcept {
  if (const auto *occurrence =
          std::get_if<PrimitiveOccurrenceKey>(&value)) {
    std::optional<PrimitiveOccurrenceKey> result;
    bool matched = false;
    for (const PrimitiveOccurrenceRemap &entry :
         remap.primitiveOccurrences) {
      if (entry.before != *occurrence) {
        continue;
      }
      if (matched) {
        return {RemapStatus::invalid, {}};
      }
      matched = true;
      result = entry.after;
    }
    if (!matched) {
      return {};
    }
    return remap.complete
               ? RemapResult{RemapStatus::mapped,
                             SceneSelectionValue{*result}}
               : RemapResult{RemapStatus::invalid, {}};
  }
  return std::visit(
      [&remap](const auto &selected) -> RemapResult {
        using Selected = std::decay_t<decltype(selected)>;
        if constexpr (std::is_same_v<Selected, std::monostate> ||
                      std::is_same_v<Selected,
                                     PrimitiveOccurrenceKey>) {
          return {};
        } else {
          return remapTypedId(remap, selected);
        }
      },
      value);
}

[[nodiscard]] bool viewportCause(SelectionCause cause) noexcept {
  return cause == SelectionCause::viewportSingle ||
         cause == SelectionCause::viewportSamePrimitiveToggle;
}

static_assert(std::is_nothrow_move_assignable_v<SceneSelectionValue>);
static_assert(std::is_nothrow_copy_assignable_v<SceneSelectionValue>);

} // namespace

SceneSelection::SceneSelection(EditorDocumentAuthority authority) noexcept
    : m_authority(authority) {}

EditorDocumentAuthority SceneSelection::authority() const noexcept {
  return m_authority;
}

const SceneSelectionValue &SceneSelection::value() const noexcept {
  return m_value;
}

SelectionRevision SceneSelection::revision() const noexcept {
  return m_revision;
}

std::uint64_t SceneSelection::documentRevision() const noexcept {
  return m_documentRevision;
}

std::uint64_t SceneSelection::sceneActivationEpoch() const noexcept {
  return m_sceneActivationEpoch;
}

std::uint64_t SceneSelection::occurrenceGeneration() const noexcept {
  return m_occurrenceGeneration;
}

SceneSelectionSetStatus SceneSelection::lastSetStatus() const noexcept {
  return m_lastStatus;
}

std::size_t SceneSelection::pendingChangeCount() const noexcept {
  return m_pendingChanges.size();
}

bool SceneSelection::apply(SceneSelectionValue value, SelectionCause cause,
                           bool requestBrowserFocus,
                           std::uint64_t document_revision,
                           std::uint64_t scene_activation_epoch,
                           std::uint64_t occurrence_generation) {
  if (value == m_value) {
    m_documentRevision = document_revision;
    m_sceneActivationEpoch = scene_activation_epoch;
    m_occurrenceGeneration = occurrence_generation;
    m_lastStatus = SceneSelectionSetStatus::unchanged;
    return false;
  }
  if (m_revision == std::numeric_limits<SelectionRevision>::max()) {
    m_lastStatus = SceneSelectionSetStatus::rejectedRevisionExhausted;
    return false;
  }

  const SelectionRevision next_revision = m_revision + 1u;
  SelectionChanged event;
  event.revision = next_revision;
  event.before = m_value;
  event.after = value;
  event.cause = cause;
  if (requestBrowserFocus &&
      !std::holds_alternative<std::monostate>(value)) {
    event.browserFocus = SceneBrowserFocusRequest{
        next_revision, value, true, true, false};
  }

  // Allocation is the only throwing mutation. Commit the authority state only
  // after the ordered event is durably owned by this frame queue.
  m_pendingChanges.push_back(std::move(event));
  m_value = std::move(value);
  m_revision = next_revision;
  m_documentRevision = document_revision;
  m_sceneActivationEpoch = scene_activation_epoch;
  m_occurrenceGeneration = occurrence_generation;
  m_lastStatus = SceneSelectionSetStatus::changed;
  return true;
}

bool SceneSelection::set(const GltfDocument &document,
                         SceneSelectionValue value, SelectionCause cause,
                         bool requestBrowserFocus) {
  const EditorDocumentAuthority document_authority =
      document.documentAuthority();
  if (std::holds_alternative<std::monostate>(value)) {
    return clear(cause);
  }
  if (std::holds_alternative<PrimitiveOccurrenceKey>(value)) {
    m_lastStatus = SceneSelectionSetStatus::rejectedStale;
    return false;
  }
  if (viewportCause(cause)) {
    m_lastStatus = SceneSelectionSetStatus::rejectedInvalid;
    return false;
  }
  const std::optional<EditorDocumentAuthority> value_authority =
      sceneSelectionAuthority(value);
  if (!value_authority) {
    m_lastStatus = SceneSelectionSetStatus::rejectedInvalid;
    return false;
  }
  if (*value_authority != document_authority ||
      (m_authority.valid() && m_authority != document_authority)) {
    m_lastStatus = SceneSelectionSetStatus::rejectedForeignAuthority;
    return false;
  }
  if (!liveSelection(document, value, nullptr)) {
    m_lastStatus = SceneSelectionSetStatus::rejectedStale;
    return false;
  }
  const bool changed =
      apply(std::move(value), cause, requestBrowserFocus,
            document.revision(), document.sceneActivationEpoch(), 0u);
  if (changed) {
    m_authority = document_authority;
  }
  return changed;
}

bool SceneSelection::setPrimitive(
    const GltfDocument &document,
    const ISceneSelectionOccurrenceLookup &occurrences,
    PrimitiveOccurrenceKey occurrence, SelectionCause cause,
    bool requestBrowserFocus) {
  const EditorDocumentAuthority document_authority =
      document.documentAuthority();
  if (m_authority.valid() && m_authority != document_authority) {
    m_lastStatus = SceneSelectionSetStatus::rejectedForeignAuthority;
    return false;
  }
  if (!liveOccurrence(document, occurrences, occurrence)) {
    m_lastStatus = SceneSelectionSetStatus::rejectedStale;
    return false;
  }
  if (viewportCause(cause) && !occurrences.visible(occurrence)) {
    m_lastStatus = SceneSelectionSetStatus::rejectedHidden;
    return false;
  }
  const bool changed = apply(
      SceneSelectionValue{occurrence}, cause, requestBrowserFocus,
      document.revision(), document.sceneActivationEpoch(),
      occurrences.generation());
  if (changed) {
    m_authority = document_authority;
  }
  return changed;
}

bool SceneSelection::clear(SelectionCause cause) {
  return apply(SceneSelectionValue{}, cause, false, 0u, 0u, 0u);
}

bool SceneSelection::toggleViewportPrimitive(
    const GltfDocument &document,
    const ISceneSelectionOccurrenceLookup &occurrences,
    PrimitiveOccurrenceKey occurrence,
    bool soleHighlightedOccurrenceMatches) {
  if (!liveOccurrence(document, occurrences, occurrence)) {
    m_lastStatus = SceneSelectionSetStatus::rejectedStale;
    return false;
  }
  if (!occurrences.visible(occurrence)) {
    m_lastStatus = SceneSelectionSetStatus::rejectedHidden;
    return false;
  }
  const auto *current = std::get_if<PrimitiveOccurrenceKey>(&m_value);
  if (soleHighlightedOccurrenceMatches && current &&
      *current == occurrence) {
    return clear(SelectionCause::viewportSamePrimitiveToggle);
  }
  return setPrimitive(document, occurrences, occurrence,
                      SelectionCause::viewportSingle, true);
}

bool SceneSelection::reconcile(
    const GltfDocument &document, const SelectionRemap *explicitRemap,
    const ISceneSelectionOccurrenceLookup *occurrences,
    SelectionCause cause) {
  const EditorDocumentAuthority target_authority =
      document.documentAuthority();
  const std::uint64_t target_revision = document.revision();
  const std::uint64_t target_scene_epoch =
      document.sceneActivationEpoch();
  if (std::holds_alternative<std::monostate>(m_value)) {
    m_authority = target_authority;
    m_documentRevision = target_revision;
    m_sceneActivationEpoch = target_scene_epoch;
    m_occurrenceGeneration = 0u;
    m_lastStatus = SceneSelectionSetStatus::unchanged;
    return false;
  }

  bool force_clear = false;
  const bool envelope_matches =
      explicitRemap &&
      explicitRemap->sourceAuthority == m_authority &&
      explicitRemap->targetAuthority == target_authority &&
      explicitRemap->sourceDocumentRevision == m_documentRevision &&
      explicitRemap->targetDocumentRevision == target_revision &&
      explicitRemap->sourceSceneActivationEpoch ==
          m_sceneActivationEpoch &&
      explicitRemap->targetSceneActivationEpoch == target_scene_epoch &&
      explicitRemap->sourceOccurrenceGeneration ==
          m_occurrenceGeneration;
  if (envelope_matches) {
    const RemapResult remapped =
        remapSelection(*explicitRemap, m_value);
    if (remapped.status == RemapStatus::mapped) {
      const bool target_is_occurrence =
          std::holds_alternative<PrimitiveOccurrenceKey>(remapped.value);
      const std::uint64_t target_occurrence_generation =
          target_is_occurrence && occurrences ? occurrences->generation()
                                              : 0u;
      const bool generation_matches =
          explicitRemap->targetOccurrenceGeneration ==
          target_occurrence_generation;
      if (generation_matches &&
          liveSelection(document, remapped.value, occurrences)) {
        const bool changed =
            apply(remapped.value, cause, false, target_revision,
                  target_scene_epoch, target_occurrence_generation);
        if (changed) {
          m_authority = target_authority;
        }
        return changed;
      }
      force_clear = true;
    } else if (remapped.status == RemapStatus::invalid) {
      force_clear = true;
    }
  }

  if (!force_clear && m_authority == target_authority &&
      liveSelection(document, m_value, occurrences)) {
    m_documentRevision = target_revision;
    m_sceneActivationEpoch = target_scene_epoch;
    m_occurrenceGeneration =
        std::holds_alternative<PrimitiveOccurrenceKey>(m_value) &&
                occurrences
            ? occurrences->generation()
            : 0u;
    m_lastStatus = SceneSelectionSetStatus::unchanged;
    return false;
  }

  const bool changed =
      apply(SceneSelectionValue{}, cause, false, target_revision,
            target_scene_epoch, 0u);
  m_authority = target_authority;
  return changed;
}

bool SceneSelection::highlightEligible(
    const GltfDocument &document,
    const ISceneSelectionOccurrenceLookup *occurrences) const noexcept {
  if (m_authority != document.documentAuthority() ||
      !liveSelection(document, m_value, occurrences) || !occurrences ||
      !currentLookup(document, *occurrences)) {
    return false;
  }
  if (const auto *node = std::get_if<NodeId>(&m_value)) {
    return occurrences->nodeHighlightEligible(*node);
  }
  if (const auto *occurrence =
          std::get_if<PrimitiveOccurrenceKey>(&m_value)) {
    return occurrences->visible(*occurrence);
  }
  return false;
}

bool SceneSelection::pickable(
    const GltfDocument &document,
    const ISceneSelectionOccurrenceLookup &occurrences,
    PrimitiveOccurrenceKey occurrence) noexcept {
  return liveOccurrence(document, occurrences, occurrence) &&
         occurrences.visible(occurrence);
}

std::vector<SelectionChanged> SceneSelection::drainChanges() {
  std::vector<SelectionChanged> result;
  result.swap(m_pendingChanges);
  return result;
}

} // namespace demo::editor
