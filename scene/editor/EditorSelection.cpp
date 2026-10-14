#include <mgif/scene_editor/EditorSelection.h>

#include <mgif/scene_editor/GltfDocument.h>

#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace demo::editor {
namespace {


enum class SelectionValidation : std::uint8_t {
  valid,
  stale,
  foreignAuthority,
};

template <typename Id>
[[nodiscard]] SelectionValidation
validateId(Id id, EditorDocumentAuthority authority) noexcept {
  if (!id.valid()) {
    return SelectionValidation::stale;
  }
  if (id.authority() != authority) {
    return SelectionValidation::foreignAuthority;
  }
  return SelectionValidation::valid;
}

template <typename Id>
[[nodiscard]] SelectionValidation
validateContainedId(const GltfDocument &document, Id id,
                    EditorDocumentAuthority authority) noexcept {
  const SelectionValidation validation = validateId(id, authority);
  if (validation != SelectionValidation::valid) {
    return validation;
  }
  return document.contains(id.untyped()) ? SelectionValidation::valid
                                         : SelectionValidation::stale;
}

template <typename... Ids>
[[nodiscard]] SelectionValidation
validateContainedIds(const GltfDocument &document,
                     EditorDocumentAuthority authority, Ids... ids) noexcept {
  SelectionValidation result = SelectionValidation::valid;
  const auto inspect = [&](auto id) noexcept {
    const SelectionValidation next =
        validateContainedId(document, id, authority);
    if (next == SelectionValidation::stale) {
      result = SelectionValidation::stale;
    } else if (next == SelectionValidation::foreignAuthority &&
               result == SelectionValidation::valid) {
      result = SelectionValidation::foreignAuthority;
    }
  };
  (inspect(ids), ...);
  return result;
}

[[nodiscard]] bool
isCanonicalEmpty(const EditorSelectionSnapshot &value) noexcept {
  return value.authority == EditorDocumentAuthority{} && value.emptyValue();
}

[[nodiscard]] SelectionValidation
validateSelection(const GltfDocument &document,
                  const EditorSelectionSnapshot &value) noexcept {
  const EditorDocumentAuthority authority = document.documentAuthority();
  if (value.authority != authority) {
    return SelectionValidation::foreignAuthority;
  }

  switch (value.value.index()) {
  case 1: {
    const auto &selection = std::get<NodeSelection>(value.value);
    SelectionValidation ids = validateContainedIds(
        document, authority, selection.scene, selection.node);
    if (ids != SelectionValidation::valid) {
      return ids;
    }
    if (selection.parent) {
      ids = validateContainedId(document, *selection.parent, authority);
      if (ids != SelectionValidation::valid) {
        return ids;
      }
    }
    return document.hasNodeOccurrence(selection.scene, selection.node,
                                      selection.parent)
               ? SelectionValidation::valid
               : SelectionValidation::stale;
  }
  case 2: {
    const auto &selection = std::get<PrimitiveSelection>(value.value);
    const SelectionValidation ids = validateContainedIds(
        document, authority, selection.scene, selection.node, selection.mesh,
        selection.primitive);
    if (ids != SelectionValidation::valid) {
      return ids;
    }
    return document.ownsPrimitiveOccurrence(selection.scene, selection.node,
                                            selection.mesh, selection.primitive)
               ? SelectionValidation::valid
               : SelectionValidation::stale;
  }
  case 3: {
    const MeshId id = std::get<MeshId>(value.value);
    return validateContainedId(document, id, authority);
  }
  case 4: {
    const MaterialId id = std::get<MaterialId>(value.value);
    return validateContainedId(document, id, authority);
  }
  case 5: {
    const CameraId id = std::get<CameraId>(value.value);
    return validateContainedId(document, id, authority);
  }
  case 6: {
    const LightId id = std::get<LightId>(value.value);
    return validateContainedId(document, id, authority);
  }
  default:
    return SelectionValidation::stale;
  }
}

static_assert(std::is_nothrow_copy_assignable_v<EditorSelectionSnapshot>);
static_assert(std::is_nothrow_move_assignable_v<EditorSelectionSnapshot>);

} // namespace

EditorSelectionSnapshot EditorSelectionSnapshot::empty() noexcept { return {}; }

EditorSelectionKind EditorSelectionSnapshot::kind() const noexcept {
  switch (value.index()) {
  case 1:
    return EditorSelectionKind::node;
  case 2:
    return EditorSelectionKind::primitive;
  case 3:
    return EditorSelectionKind::mesh;
  case 4:
    return EditorSelectionKind::material;
  case 5:
    return EditorSelectionKind::camera;
  case 6:
    return EditorSelectionKind::light;
  default:
    return EditorSelectionKind::none;
  }
}

bool EditorSelectionSnapshot::emptyValue() const noexcept {
  return std::holds_alternative<std::monostate>(value);
}

bool EditorSelectionTransition::accepted() const noexcept {
  return status == EditorSelectionTransitionStatus::changed ||
         status == EditorSelectionTransitionStatus::unchanged;
}

bool EditorSelectionTransition::changed() const noexcept {
  return status == EditorSelectionTransitionStatus::changed;
}

#if defined(MGIF_LEGACY_SELECTION_TESTING)
bool MaterialClipboardResolution::pasteEnabled() const noexcept {
  return status != MaterialClipboardStatus::malformed;
}

bool MaterialClipboardResolution::applicable() const noexcept {
  return status == MaterialClipboardStatus::resolved && material.has_value();
}

std::string encodeMaterialClipboardIndex(std::size_t documentIndex) {
  std::string result{kMaterialClipboardPrefix};
  result += std::to_string(documentIndex);
  return result;
}

std::optional<std::size_t>
decodeMaterialClipboardIndex(std::string_view text) noexcept {
  if (!text.starts_with(kMaterialClipboardPrefix)) {
    return std::nullopt;
  }

  text.remove_prefix(kMaterialClipboardPrefix.size());
  if (text.empty()) {
    return std::nullopt;
  }

  std::size_t result = 0u;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      return std::nullopt;
    }
    const std::size_t value = static_cast<std::size_t>(digit - '0');
    if (result > (std::numeric_limits<std::size_t>::max() - value) / 10u) {
      return std::nullopt;
    }
    result = result * 10u + value;
  }
  return result;
}

MaterialClipboardResolution
resolveMaterialClipboard(const GltfDocument &document,
                         std::string_view text) noexcept {
  const std::optional<std::size_t> index = decodeMaterialClipboardIndex(text);
  if (!index) {
    return {};
  }

  MaterialClipboardResolution result;
  result.status = MaterialClipboardStatus::outOfRange;
  result.documentIndex = index;
  if (*index >= document.objectCount(EditorObjectKind::material)) {
    return result;
  }

  const std::optional<MaterialId> material =
      MaterialId::from(document.idAt(EditorObjectKind::material, *index));
  if (!material || !document.contains(material->untyped())) {
    return result;
  }
  result.status = MaterialClipboardStatus::resolved;
  result.material = material;
  return result;
}
#endif

const EditorSelectionSnapshot &EditorSelectionState::snapshot() const noexcept {
  if (m_authority != nullptr) {
    return m_authority->snapshot();
  }
#if defined(MGIF_LEGACY_SELECTION_TESTING)
  return m_value;
#else
  static const EditorSelectionSnapshot empty =
      EditorSelectionSnapshot::empty();
  return empty;
#endif
}

void EditorSelectionState::bindAuthority(
    ILegacySelectionAuthority &authority) noexcept {
  m_authority = &authority;
}

void EditorSelectionState::unbindAuthority() noexcept {
  m_authority = nullptr;
}

bool EditorSelectionState::authorityBound() const noexcept {
  return m_authority != nullptr;
}

std::uint64_t EditorSelectionState::revision() const noexcept {
  if (m_authority != nullptr) {
    return m_authority->revision();
  }
#if defined(MGIF_LEGACY_SELECTION_TESTING)
  return m_revision;
#else
  return 0u;
#endif
}

const std::optional<EditorSelectionEvent> &
EditorSelectionState::lastEvent() const noexcept {
  if (m_authority != nullptr) {
    return m_authority->lastEvent();
  }
#if defined(MGIF_LEGACY_SELECTION_TESTING)
  return m_lastEvent;
#else
  static const std::optional<EditorSelectionEvent> empty;
  return empty;
#endif
}

#if defined(MGIF_LEGACY_SELECTION_TESTING)
EditorSelectionTransition
EditorSelectionState::apply(EditorSelectionSnapshot value) noexcept {
  if (value == m_value) {
    return {EditorSelectionTransitionStatus::unchanged, std::nullopt};
  }
  if (m_revision == std::numeric_limits<std::uint64_t>::max()) {
    return {EditorSelectionTransitionStatus::rejectedRevisionExhausted,
            std::nullopt};
  }

  EditorSelectionEvent event;
  event.revision = m_revision + 1u;
  event.previous = m_value;
  event.current = value;
  m_value = std::move(value);
  m_revision = event.revision;
  m_lastEvent = event;
  return {EditorSelectionTransitionStatus::changed, std::move(event)};
}
#endif

EditorSelectionTransition EditorSelectionState::clearTransition() noexcept {
  if (m_authority != nullptr) {
    return m_authority->clearTransition();
  }
#if defined(MGIF_LEGACY_SELECTION_TESTING)
  return apply(EditorSelectionSnapshot::empty());
#else
  return {EditorSelectionTransitionStatus::rejectedStale, std::nullopt};
#endif
}

EditorSelectionTransition
EditorSelectionState::transition(const GltfDocument &document,
                                 EditorSelectionSnapshot value) noexcept {
  if (m_authority != nullptr) {
    return m_authority->transition(document, std::move(value));
  }
#if defined(MGIF_LEGACY_SELECTION_TESTING)
  if (value.emptyValue()) {
    if (!isCanonicalEmpty(value)) {
      return {EditorSelectionTransitionStatus::rejectedNonCanonicalEmpty,
              std::nullopt};
    }
    return clearTransition();
  }

  switch (validateSelection(document, value)) {
  case SelectionValidation::valid:
    return apply(std::move(value));
  case SelectionValidation::foreignAuthority:
    return {EditorSelectionTransitionStatus::rejectedForeignAuthority,
            std::nullopt};
  case SelectionValidation::stale:
    return {EditorSelectionTransitionStatus::rejectedStale, std::nullopt};
  }
  return {EditorSelectionTransitionStatus::rejectedStale, std::nullopt};
#else
  (void)document;
  (void)value;
  return {EditorSelectionTransitionStatus::rejectedStale, std::nullopt};
#endif
}

void EditorSelectionState::clear() noexcept { (void)clearTransition(); }

bool EditorSelectionState::select(const GltfDocument &document,
                                  EditorSelectionSnapshot value) noexcept {
  return transition(document, std::move(value)).accepted();
}

SelectionRestoreStatus
EditorSelectionState::restore(const GltfDocument &document,
                              const EditorSelectionSnapshot &value) noexcept {
  if (m_authority != nullptr) {
    return m_authority->restore(document, value);
  }
#if defined(MGIF_LEGACY_SELECTION_TESTING)
  if (value.emptyValue()) {
    (void)clearTransition();
    return isCanonicalEmpty(value) ? SelectionRestoreStatus::clearedEmpty
                                   : SelectionRestoreStatus::clearedStale;
  }

  const SelectionValidation validation = validateSelection(document, value);
  if (validation == SelectionValidation::valid) {
    (void)apply(value);
    return SelectionRestoreStatus::restored;
  }

  (void)clearTransition();
  return validation == SelectionValidation::foreignAuthority
             ? SelectionRestoreStatus::clearedForeignAuthority
             : SelectionRestoreStatus::clearedStale;
#else
  (void)document;
  (void)value;
  return SelectionRestoreStatus::clearedStale;
#endif
}

SelectionRestoreStatus
EditorSelectionState::revalidate(const GltfDocument &document) noexcept {
  if (m_authority != nullptr) {
    return m_authority->revalidate(document);
  }
#if defined(MGIF_LEGACY_SELECTION_TESTING)
  const EditorSelectionSnapshot current = m_value;
  return restore(document, current);
#else
  (void)document;
  return SelectionRestoreStatus::clearedStale;
#endif
}

} // namespace demo::editor
