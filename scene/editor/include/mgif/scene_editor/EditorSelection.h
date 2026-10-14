#pragma once

#include <mgif/scene_editor/EditorSelectionTypes.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace demo::editor {

class GltfDocument;

enum class EditorSelectionKind : std::uint8_t {
  none = 0,
  node,
  primitive,
  mesh,
  material,
  camera,
  light
};

struct NodeSelection final {
  SceneId scene;
  NodeId node;
  std::optional<NodeId> parent;

  auto operator<=>(const NodeSelection &) const = default;
};

struct PrimitiveSelection final {
  SceneId scene;
  NodeId node;
  PrimitiveId primitive;
  MeshId mesh;
  std::uint32_t instanceOrdinal{0};

  auto operator<=>(const PrimitiveSelection &) const = default;
};

using EditorSelectionValue =
    std::variant<std::monostate, NodeSelection, PrimitiveSelection, MeshId,
                 MaterialId, CameraId, LightId>;

struct EditorSelectionSnapshot final {
  EditorDocumentAuthority authority{};
  EditorSelectionValue value{};

  [[nodiscard]] static EditorSelectionSnapshot empty() noexcept;
  [[nodiscard]] EditorSelectionKind kind() const noexcept;
  [[nodiscard]] bool emptyValue() const noexcept;

  auto operator<=>(const EditorSelectionSnapshot &) const = default;
};

enum class SelectionRestoreStatus : std::uint8_t {
  restored = 0,
  clearedEmpty,
  clearedStale,
  clearedForeignAuthority,
};

enum class EditorSelectionTransitionStatus : std::uint8_t {
  changed = 0,
  unchanged,
  rejectedStale,
  rejectedForeignAuthority,
  rejectedNonCanonicalEmpty,
  rejectedRevisionExhausted,
};

struct EditorSelectionEvent final {
  std::uint64_t revision{0};
  EditorSelectionSnapshot previous{};
  EditorSelectionSnapshot current{};

  bool operator==(const EditorSelectionEvent &) const = default;
};

struct EditorSelectionTransition final {
  EditorSelectionTransitionStatus status{
      EditorSelectionTransitionStatus::unchanged};
  std::optional<EditorSelectionEvent> event;

  [[nodiscard]] bool accepted() const noexcept;
  [[nodiscard]] bool changed() const noexcept;

  bool operator==(const EditorSelectionTransition &) const = default;
};

#if defined(MGIF_LEGACY_SELECTION_TESTING)
enum class MaterialClipboardStatus : std::uint8_t {
  malformed = 0,
  outOfRange,
  resolved,
};

struct MaterialClipboardResolution final {
  MaterialClipboardStatus status{MaterialClipboardStatus::malformed};
  std::optional<std::size_t> documentIndex;
  std::optional<MaterialId> material;

  [[nodiscard]] bool pasteEnabled() const noexcept;
  [[nodiscard]] bool applicable() const noexcept;

  bool operator==(const MaterialClipboardResolution &) const = default;
};

inline constexpr std::string_view kMaterialClipboardPrefix = "gltf_material:";

[[nodiscard]] std::string
encodeMaterialClipboardIndex(std::size_t documentIndex);
[[nodiscard]] std::optional<std::size_t>
decodeMaterialClipboardIndex(std::string_view text) noexcept;
[[nodiscard]] MaterialClipboardResolution
resolveMaterialClipboard(const GltfDocument &document,
                         std::string_view text) noexcept;
#endif

// Production binding implemented by the app owner of frozen SceneSelection.
// The interface deliberately preserves the P4 command/history call surface
// without giving scene core a second selection value or revision authority.
class ILegacySelectionAuthority {
public:
  virtual ~ILegacySelectionAuthority() = default;
  [[nodiscard]] virtual const EditorSelectionSnapshot &snapshot() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t revision() const noexcept = 0;
  [[nodiscard]] virtual const std::optional<EditorSelectionEvent> &
  lastEvent() const noexcept = 0;
  [[nodiscard]] virtual EditorSelectionTransition clearTransition() noexcept = 0;
  [[nodiscard]] virtual EditorSelectionTransition
  transition(const GltfDocument &document,
             EditorSelectionSnapshot value) noexcept = 0;
  [[nodiscard]] virtual SelectionRestoreStatus
  restore(const GltfDocument &document,
          const EditorSelectionSnapshot &value) noexcept = 0;
  [[nodiscard]] virtual SelectionRestoreStatus
  revalidate(const GltfDocument &document) noexcept = 0;
};

class EditorSelectionState final {
public:
  void bindAuthority(ILegacySelectionAuthority &authority) noexcept;
  void unbindAuthority() noexcept;
  [[nodiscard]] bool authorityBound() const noexcept;
  [[nodiscard]] const EditorSelectionSnapshot &snapshot() const noexcept;
  [[nodiscard]] std::uint64_t revision() const noexcept;
  [[nodiscard]] const std::optional<EditorSelectionEvent> &
  lastEvent() const noexcept;

  [[nodiscard]] EditorSelectionTransition clearTransition() noexcept;
  [[nodiscard]] EditorSelectionTransition
  transition(const GltfDocument &document,
             EditorSelectionSnapshot value) noexcept;

  void clear() noexcept;
  [[nodiscard]] bool select(const GltfDocument &document,
                            EditorSelectionSnapshot value) noexcept;
  [[nodiscard]] SelectionRestoreStatus
  restore(const GltfDocument &document,
          const EditorSelectionSnapshot &value) noexcept;
  [[nodiscard]] SelectionRestoreStatus
  revalidate(const GltfDocument &document) noexcept;

private:
#if defined(MGIF_LEGACY_SELECTION_TESTING)
  [[nodiscard]] EditorSelectionTransition
  apply(EditorSelectionSnapshot value) noexcept;
#endif

  ILegacySelectionAuthority *m_authority{nullptr};
#if defined(MGIF_LEGACY_SELECTION_TESTING)
  // Test-only ABI variant for frozen pre-P6 core tests. Production has no
  // independent selection value, revision, or event storage.
  EditorSelectionSnapshot m_value{};
  std::uint64_t m_revision{0};
  std::optional<EditorSelectionEvent> m_lastEvent;
#endif
};

} // namespace demo::editor
