#pragma once

#include <mgif/scene_editor/EditorObjectId.h>

#include <compare>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace demo::editor {

using SelectionRevision = std::uint64_t;

struct PrimitiveOccurrenceKey final {
  SceneId scene;
  NodeId node;
  PrimitiveId primitive;
  std::uint32_t instanceOrdinal{0};

  auto operator<=>(const PrimitiveOccurrenceKey &) const = default;
};

using SceneSelectionValue =
    std::variant<std::monostate, NodeId, PrimitiveOccurrenceKey, MeshId,
                 MaterialId, CameraId, LightId>;

enum class SceneSelectionKind : std::uint8_t {
  none = 0,
  node,
  primitive,
  mesh,
  material,
  camera,
  light,
};

enum class SelectionCause : std::uint8_t {
  hierarchy = 0,
  sceneList,
  inspector,
  viewportSingle,
  viewportSamePrimitiveToggle,
  escape,
  documentMutation,
  documentReplacement,
  undoRedoRestore,
  programmatic,
};

struct SceneBrowserFocusRequest final {
  SelectionRevision selectionRevision{0};
  SceneSelectionValue target{};
  bool revealParents{true};
  bool scrollToTarget{true};
  bool selectTab{false};

  bool operator==(const SceneBrowserFocusRequest &) const = default;
};

struct SelectionChanged final {
  SelectionRevision revision{0};
  SceneSelectionValue before{};
  SceneSelectionValue after{};
  SelectionCause cause{SelectionCause::programmatic};
  std::optional<SceneBrowserFocusRequest> browserFocus;

  bool operator==(const SelectionChanged &) const = default;
};

struct SelectionIdRemap final {
  EditorObjectId before;
  EditorObjectId after;

  auto operator<=>(const SelectionIdRemap &) const = default;
};

struct PrimitiveOccurrenceRemap final {
  PrimitiveOccurrenceKey before;
  PrimitiveOccurrenceKey after;

  auto operator<=>(const PrimitiveOccurrenceRemap &) const = default;
};

struct SelectionRemap final {
  EditorDocumentAuthority sourceAuthority;
  EditorDocumentAuthority targetAuthority;
  std::uint64_t sourceDocumentRevision{0};
  std::uint64_t targetDocumentRevision{0};
  std::uint64_t sourceSceneActivationEpoch{0};
  std::uint64_t targetSceneActivationEpoch{0};
  std::uint64_t sourceOccurrenceGeneration{0};
  std::uint64_t targetOccurrenceGeneration{0};
  std::vector<SelectionIdRemap> ids;
  std::vector<PrimitiveOccurrenceRemap> primitiveOccurrences;
  bool complete{false};

  bool operator==(const SelectionRemap &) const = default;
};

[[nodiscard]] inline SceneSelectionKind
sceneSelectionKind(const SceneSelectionValue &value) noexcept {
  switch (value.index()) {
  case 1:
    return SceneSelectionKind::node;
  case 2:
    return SceneSelectionKind::primitive;
  case 3:
    return SceneSelectionKind::mesh;
  case 4:
    return SceneSelectionKind::material;
  case 5:
    return SceneSelectionKind::camera;
  case 6:
    return SceneSelectionKind::light;
  default:
    return SceneSelectionKind::none;
  }
}

[[nodiscard]] inline std::optional<EditorDocumentAuthority>
sceneSelectionAuthority(const SceneSelectionValue &value) noexcept {
  if (const auto *occurrence =
          std::get_if<PrimitiveOccurrenceKey>(&value)) {
    const EditorDocumentAuthority authority = occurrence->scene.authority();
    if (!occurrence->scene.valid() || !occurrence->node.valid() ||
        !occurrence->primitive.valid() || !authority.valid() ||
        occurrence->node.authority() != authority ||
        occurrence->primitive.authority() != authority) {
      return std::nullopt;
    }
    return authority;
  }
  if (const auto *node = std::get_if<NodeId>(&value)) {
    return node->valid()
               ? std::optional<EditorDocumentAuthority>{node->authority()}
               : std::nullopt;
  }
  if (const auto *mesh = std::get_if<MeshId>(&value)) {
    return mesh->valid()
               ? std::optional<EditorDocumentAuthority>{mesh->authority()}
               : std::nullopt;
  }
  if (const auto *material = std::get_if<MaterialId>(&value)) {
    return material->valid()
               ? std::optional<EditorDocumentAuthority>{
                     material->authority()}
               : std::nullopt;
  }
  if (const auto *camera = std::get_if<CameraId>(&value)) {
    return camera->valid()
               ? std::optional<EditorDocumentAuthority>{camera->authority()}
               : std::nullopt;
  }
  if (const auto *light = std::get_if<LightId>(&value)) {
    return light->valid()
               ? std::optional<EditorDocumentAuthority>{light->authority()}
               : std::nullopt;
  }
  return std::nullopt;
}

} // namespace demo::editor
