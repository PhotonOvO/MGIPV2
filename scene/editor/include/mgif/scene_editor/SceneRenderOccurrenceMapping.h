#pragma once

#include <mgif/scene_editor/EditorPickingTypes.h>

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace demo::editor {

enum class SceneRenderOccurrenceMappingError : std::uint8_t {
  none = 0,
  invalidFence,
  invalidGeneration,
  capacityExceeded,
  invalidRecord,
  foreignAuthority,
  wrongScene,
  duplicateId,
  duplicateKey,
  nonCanonicalOrdinal,
  invalidNodeTopology,
  duplicateNode,
  unknownNode,
  allocationFailure,
};

struct SceneRenderNodeRecord final {
  NodeId node;
  std::optional<NodeId> parent;
  std::uint32_t depth{0};

  auto operator<=>(const SceneRenderNodeRecord &) const = default;
};

class SceneRenderOccurrenceMapping;
using SceneRenderOccurrenceSnapshot =
    std::shared_ptr<const SceneRenderOccurrenceMapping>;

struct SceneRenderOccurrenceMappingCreateResult final {
  SceneRenderOccurrenceMappingError error{
      SceneRenderOccurrenceMappingError::invalidFence};
  std::string_view message;
  SceneRenderOccurrenceSnapshot snapshot;

  [[nodiscard]] bool succeeded() const noexcept {
    return error == SceneRenderOccurrenceMappingError::none &&
           static_cast<bool>(snapshot);
  }
};

class SceneRenderOccurrenceMapping final {
public:
  [[nodiscard]] static SceneRenderOccurrenceMappingCreateResult
  create(EditorSceneFence fence, std::uint64_t generation,
         std::vector<SceneRenderNodeRecord> nodes,
         std::vector<RenderOccurrenceRecord> records) noexcept;

  [[nodiscard]] const EditorSceneFence &fence() const noexcept;
  [[nodiscard]] std::uint64_t generation() const noexcept;
  [[nodiscard]] std::span<const RenderOccurrenceRecord> records() const noexcept;
  [[nodiscard]] std::span<const SceneRenderNodeRecord> nodes() const noexcept;

  [[nodiscard]] std::optional<RenderOccurrenceRecord>
  resolve(RenderOccurrenceId id) const noexcept;
  [[nodiscard]] std::span<const RenderOccurrenceId>
  occurrences(NodeId node) const noexcept;
  [[nodiscard]] std::optional<RenderOccurrenceId>
  occurrence(PrimitiveOccurrenceKey key) const noexcept;
  [[nodiscard]] std::optional<FramebufferPickToken>
  framebufferToken(RenderOccurrenceId id) const noexcept;
  [[nodiscard]] std::optional<RenderOccurrenceRecord>
  resolveFramebufferToken(FramebufferPickToken token) const noexcept;

  [[nodiscard]] std::optional<EditorHighlightState>
  highlight(const SceneSelectionValue &selection,
            SelectionRevision selectionRevision) const;

private:
  struct NodeOccurrences final {
    NodeId node;
    std::vector<RenderOccurrenceId> ids;
  };

  SceneRenderOccurrenceMapping(EditorSceneFence fence, std::uint64_t generation,
                               std::vector<SceneRenderNodeRecord> nodes,
                               std::vector<RenderOccurrenceRecord> records,
                               std::vector<NodeOccurrences> byNode) noexcept;

  [[nodiscard]] const RenderOccurrenceRecord *
  findRecord(RenderOccurrenceId id) const noexcept;
  [[nodiscard]] const RenderOccurrenceRecord *
  findRecord(PrimitiveOccurrenceKey key) const noexcept;

  EditorSceneFence m_fence;
  std::uint64_t m_generation{0};
  std::vector<SceneRenderNodeRecord> m_nodes;
  std::vector<RenderOccurrenceRecord> m_records;
  std::vector<NodeOccurrences> m_byNode;
};

#if defined(MGIF_EDITOR_PICKING_TESTING)
namespace testing {
void failNextOccurrenceMappingAllocation() noexcept;
}
#endif

} // namespace demo::editor
