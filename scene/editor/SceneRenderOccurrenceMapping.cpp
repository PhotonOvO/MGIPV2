#include <mgif/scene_editor/SceneRenderOccurrenceMapping.h>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace demo::editor {
namespace {

#if defined(MGIF_EDITOR_PICKING_TESTING)
bool g_failNextAllocation{false};
#endif

[[nodiscard]] bool sameDefinition(const PrimitiveOccurrenceKey &left,
                                  const PrimitiveOccurrenceKey &right) noexcept {
  return left.scene == right.scene && left.node == right.node &&
         left.primitive == right.primitive;
}

[[nodiscard]] SceneRenderOccurrenceMappingCreateResult
failure(SceneRenderOccurrenceMappingError error,
        std::string_view message) noexcept {
  return {error, message, {}};
}

} // namespace

SceneRenderOccurrenceMapping::SceneRenderOccurrenceMapping(
    EditorSceneFence fence, std::uint64_t generation,
    std::vector<SceneRenderNodeRecord> nodes,
    std::vector<RenderOccurrenceRecord> records,
    std::vector<NodeOccurrences> byNode) noexcept
    : m_fence(std::move(fence)), m_generation(generation),
      m_nodes(std::move(nodes)), m_records(std::move(records)),
      m_byNode(std::move(byNode)) {}

SceneRenderOccurrenceMappingCreateResult SceneRenderOccurrenceMapping::create(
    EditorSceneFence fence, std::uint64_t generation,
    std::vector<SceneRenderNodeRecord> nodes,
    std::vector<RenderOccurrenceRecord> records) noexcept {
  if (!fence.valid()) {
    return failure(SceneRenderOccurrenceMappingError::invalidFence,
                   "occurrence mapping fence is invalid");
  }
  if (generation == 0) {
    return failure(SceneRenderOccurrenceMappingError::invalidGeneration,
                   "occurrence mapping generation is zero");
  }
  if (records.size() >=
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return failure(SceneRenderOccurrenceMappingError::capacityExceeded,
                   "occurrence mapping exceeds framebuffer token capacity");
  }

  try {
#if defined(MGIF_EDITOR_PICKING_TESTING)
    if (g_failNextAllocation) {
      g_failNextAllocation = false;
      throw std::bad_alloc{};
    }
#endif
    std::vector<NodeId> topology_stack;
    topology_stack.reserve(nodes.size());
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      const SceneRenderNodeRecord &node = nodes[index];
      if (!node.node.valid() || node.node.authority() != fence.authority ||
          (node.parent &&
           (!node.parent->valid() ||
            node.parent->authority() != fence.authority))) {
        return failure(SceneRenderOccurrenceMappingError::invalidNodeTopology,
                       "node topology contains an invalid or foreign ID");
      }
      for (std::size_t previous = 0; previous < index; ++previous) {
        if (nodes[previous].node == node.node) {
          return failure(SceneRenderOccurrenceMappingError::duplicateNode,
                         "node topology repeats a stable node ID");
        }
      }
      if (node.depth == 0) {
        if (node.parent) {
          return failure(SceneRenderOccurrenceMappingError::invalidNodeTopology,
                         "root topology node has a parent");
        }
        topology_stack.clear();
        topology_stack.push_back(node.node);
        continue;
      }
      if (!node.parent || node.depth > topology_stack.size() ||
          topology_stack[node.depth - 1u] != *node.parent) {
        return failure(SceneRenderOccurrenceMappingError::invalidNodeTopology,
                       "node topology is not canonical parent preorder");
      }
      topology_stack.resize(node.depth);
      topology_stack.push_back(node.node);
    }

    std::vector<NodeOccurrences> by_node;
    by_node.reserve(records.size());
    for (std::size_t index = 0; index < records.size(); ++index) {
      const RenderOccurrenceRecord &record = records[index];
      if (!record.id.valid() || !record.key.scene.valid() ||
          !record.key.node.valid() || !record.key.primitive.valid()) {
        return failure(SceneRenderOccurrenceMappingError::invalidRecord,
                       "occurrence record identity is invalid");
      }
      const bool foreign =
          record.key.scene.authority() != fence.authority ||
          record.key.node.authority() != fence.authority ||
          record.key.primitive.authority() != fence.authority;
      if (foreign) {
        return failure(SceneRenderOccurrenceMappingError::foreignAuthority,
                       "occurrence record belongs to another authority");
      }
      if (record.key.scene != fence.scene) {
        return failure(SceneRenderOccurrenceMappingError::wrongScene,
                       "occurrence record belongs to another scene");
      }
      const bool known_node = std::any_of(
          nodes.begin(), nodes.end(), [&](const SceneRenderNodeRecord &node) {
            return node.node == record.key.node;
          });
      if (!known_node) {
        return failure(SceneRenderOccurrenceMappingError::unknownNode,
                       "occurrence record references unknown node topology");
      }
      for (std::size_t previous = 0; previous < index; ++previous) {
        if (records[previous].id.slot == record.id.slot) {
          return failure(SceneRenderOccurrenceMappingError::duplicateId,
                         "occurrence mapping repeats a live ID slot");
        }
        if (records[previous].key == record.key) {
          return failure(SceneRenderOccurrenceMappingError::duplicateKey,
                         "occurrence mapping repeats an occurrence key");
        }
      }

      auto group = std::find_if(
          by_node.begin(), by_node.end(), [&](const NodeOccurrences &candidate) {
            return candidate.node == record.key.node;
          });
      if (group == by_node.end()) {
        by_node.push_back(NodeOccurrences{record.key.node, {record.id}});
      } else {
        group->ids.push_back(record.id);
      }
    }

    // For each scene/node/primitive definition, publication order is the
    // canonical ordinal sequence 0,1,... even when definitions interleave.
    for (std::size_t index = 0; index < records.size(); ++index) {
      std::uint32_t expected_ordinal = 0;
      for (std::size_t previous = 0; previous < index; ++previous) {
        if (sameDefinition(records[index].key, records[previous].key)) {
          ++expected_ordinal;
        }
      }
      if (records[index].key.instanceOrdinal != expected_ordinal) {
        return failure(SceneRenderOccurrenceMappingError::nonCanonicalOrdinal,
                       "occurrence publication order does not match its ordinal");
      }
    }

    auto *owned = new SceneRenderOccurrenceMapping(
        std::move(fence), generation, std::move(nodes), std::move(records),
        std::move(by_node));
    SceneRenderOccurrenceSnapshot snapshot{owned};
    return {SceneRenderOccurrenceMappingError::none, {}, std::move(snapshot)};
  } catch (const std::bad_alloc &) {
    return failure(SceneRenderOccurrenceMappingError::allocationFailure,
                   "out of memory while sealing occurrence mapping");
  } catch (...) {
    return failure(SceneRenderOccurrenceMappingError::invalidRecord,
                   "unexpected occurrence mapping construction failure");
  }
}

const EditorSceneFence &SceneRenderOccurrenceMapping::fence() const noexcept {
  return m_fence;
}

std::uint64_t SceneRenderOccurrenceMapping::generation() const noexcept {
  return m_generation;
}

std::span<const RenderOccurrenceRecord>
SceneRenderOccurrenceMapping::records() const noexcept {
  return m_records;
}

std::span<const SceneRenderNodeRecord>
SceneRenderOccurrenceMapping::nodes() const noexcept {
  return m_nodes;
}

const RenderOccurrenceRecord *SceneRenderOccurrenceMapping::findRecord(
    RenderOccurrenceId id) const noexcept {
  // Renderer publication assigns slot = record index + 1. Keep the fallback
  // for externally constructed/test snapshots whose valid IDs are sparse or
  // deliberately reordered.
  if (id.valid() && id.slot > 0u) {
    const std::size_t index = static_cast<std::size_t>(id.slot - 1u);
    if (index < m_records.size() && m_records[index].id == id)
      return &m_records[index];
  }
  const auto found = std::find_if(
      m_records.begin(), m_records.end(),
      [id](const RenderOccurrenceRecord &record) { return record.id == id; });
  return found == m_records.end() ? nullptr : &*found;
}

const RenderOccurrenceRecord *SceneRenderOccurrenceMapping::findRecord(
    PrimitiveOccurrenceKey key) const noexcept {
  const auto found = std::find_if(
      m_records.begin(), m_records.end(),
      [&key](const RenderOccurrenceRecord &record) { return record.key == key; });
  return found == m_records.end() ? nullptr : &*found;
}

std::optional<RenderOccurrenceRecord>
SceneRenderOccurrenceMapping::resolve(RenderOccurrenceId id) const noexcept {
  const RenderOccurrenceRecord *record = findRecord(id);
  return record ? std::optional<RenderOccurrenceRecord>{*record} : std::nullopt;
}

std::span<const RenderOccurrenceId>
SceneRenderOccurrenceMapping::occurrences(NodeId node) const noexcept {
  const auto found = std::find_if(
      m_byNode.begin(), m_byNode.end(),
      [node](const NodeOccurrences &entry) { return entry.node == node; });
  return found == m_byNode.end() ? std::span<const RenderOccurrenceId>{}
                                : std::span<const RenderOccurrenceId>{found->ids};
}

std::optional<RenderOccurrenceId> SceneRenderOccurrenceMapping::occurrence(
    PrimitiveOccurrenceKey key) const noexcept {
  const RenderOccurrenceRecord *record = findRecord(key);
  return record ? std::optional<RenderOccurrenceId>{record->id} : std::nullopt;
}

std::optional<FramebufferPickToken>
SceneRenderOccurrenceMapping::framebufferToken(RenderOccurrenceId id) const
    noexcept {
  for (std::size_t index = 0; index < m_records.size(); ++index) {
    if (m_records[index].id == id) {
      return FramebufferPickToken{static_cast<std::uint32_t>(index + 1u)};
    }
  }
  return std::nullopt;
}

std::optional<RenderOccurrenceRecord>
SceneRenderOccurrenceMapping::resolveFramebufferToken(
    FramebufferPickToken token) const noexcept {
  if (!token.valid()) {
    return std::nullopt;
  }
  const std::uint64_t index = static_cast<std::uint64_t>(token.value) - 1u;
  return index < m_records.size()
             ? std::optional<RenderOccurrenceRecord>{
                   m_records[static_cast<std::size_t>(index)]}
             : std::nullopt;
}

std::optional<EditorHighlightState> SceneRenderOccurrenceMapping::highlight(
    const SceneSelectionValue &selection,
    SelectionRevision selectionRevision) const {
  const std::optional<EditorDocumentAuthority> authority =
      sceneSelectionAuthority(selection);
  if (authority && *authority != m_fence.authority) {
    return std::nullopt;
  }

  EditorHighlightState state;
  state.fence = m_fence;
  state.selectionRevision = selectionRevision;
  if (const auto *node = std::get_if<NodeId>(&selection)) {
    const auto selected = std::find_if(
        m_nodes.begin(), m_nodes.end(),
        [node](const SceneRenderNodeRecord &candidate) {
          return candidate.node == *node;
        });
    if (selected == m_nodes.end()) {
      return std::nullopt;
    }
    const std::uint32_t selected_depth = selected->depth;
    for (auto current = selected; current != m_nodes.end(); ++current) {
      if (current != selected && current->depth <= selected_depth) {
        break;
      }
      for (const RenderOccurrenceId id : occurrences(current->node)) {
        const RenderOccurrenceRecord *record = findRecord(id);
        if (record == nullptr || !record->visible) {
          continue;
        }
        if (std::find(state.occurrences.begin(), state.occurrences.end(), id) ==
            state.occurrences.end()) {
          state.occurrences.push_back(id);
        }
      }
    }
  } else if (const auto *primitive =
                 std::get_if<PrimitiveOccurrenceKey>(&selection)) {
    const RenderOccurrenceRecord *record = findRecord(*primitive);
    if (record == nullptr) {
      return std::nullopt;
    }
    if (record->visible) {
      state.occurrences.push_back(record->id);
    }
  }
  return state.valid() ? std::optional<EditorHighlightState>{std::move(state)}
                       : std::nullopt;
}

#if defined(MGIF_EDITOR_PICKING_TESTING)
namespace testing {
void failNextOccurrenceMappingAllocation() noexcept {
  g_failNextAllocation = true;
}
} // namespace testing
#endif

} // namespace demo::editor
