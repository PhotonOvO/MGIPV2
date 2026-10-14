#pragma once

#include <mgif/scene_editor/SceneChangeSet.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace demo {
struct SceneAsset;
struct SceneAssetView;
}

namespace demo::editor {

class GltfDocument;

struct SceneRuntimeMappingEntry final {
  EditorObjectId documentId{};
  std::uint32_t sceneAssetIndex{std::numeric_limits<std::uint32_t>::max()};
  std::uint64_t projectionIdentity{
      std::numeric_limits<std::uint64_t>::max()};
  // Source-compatibility alias for the deterministic projection identity.
  std::uint64_t runtimeIndex{std::numeric_limits<std::uint64_t>::max()};

  bool operator==(const SceneRuntimeMappingEntry &) const = default;
};

struct SceneRuntimeMappingAlias final {
  EditorObjectId documentId{};
  EditorObjectKind kind{EditorObjectKind::invalid};
  std::uint32_t sceneAssetIndex{std::numeric_limits<std::uint32_t>::max()};

  bool operator==(const SceneRuntimeMappingAlias &) const = default;
};

struct SceneRuntimeMappingFence final {
  EditorDocumentAuthority authority{};
  std::uint64_t appliedDocumentRevision{0};
  std::optional<StableSceneId> activeScene;
  std::uint64_t sceneActivationEpoch{0};
  std::uint64_t rootExternalStateSerial{0};
  SceneDigest projectionDigest{};
  std::uint64_t mappingEpoch{0};

  bool operator==(const SceneRuntimeMappingFence &) const = default;
};

class SceneRuntimeMapping final {
public:
  static constexpr std::uint32_t kInvalidIndex =
      std::numeric_limits<std::uint32_t>::max();
  static constexpr std::uint64_t kInvalidProjectionIdentity =
      std::numeric_limits<std::uint64_t>::max();
  static constexpr std::uint64_t kInvalidRuntimeIndex =
      kInvalidProjectionIdentity;

  [[nodiscard]] static std::optional<SceneRuntimeMapping>
  create(SceneRuntimeMappingFence fence,
         std::vector<SceneRuntimeMappingEntry> entries,
         std::string *error = nullptr);
  [[nodiscard]] static std::optional<SceneRuntimeMapping>
  create(SceneRuntimeMappingFence fence,
         std::vector<SceneRuntimeMappingEntry> entries,
         std::vector<SceneRuntimeMappingAlias> aliases,
         std::string *error = nullptr);

  [[nodiscard]] static std::uint64_t
  deriveProjectionIdentity(EditorObjectId documentId) noexcept;

  [[nodiscard]] const SceneRuntimeMappingFence &fence() const noexcept {
    return m_fence;
  }
  [[nodiscard]] EditorDocumentAuthority authority() const noexcept {
    return m_fence.authority;
  }
  [[nodiscard]] std::uint64_t appliedDocumentRevision() const noexcept {
    return m_fence.appliedDocumentRevision;
  }
  [[nodiscard]] std::optional<StableSceneId> activeScene() const noexcept {
    return m_fence.activeScene;
  }
  [[nodiscard]] std::uint64_t sceneActivationEpoch() const noexcept {
    return m_fence.sceneActivationEpoch;
  }
  [[nodiscard]] std::uint64_t rootExternalStateSerial() const noexcept {
    return m_fence.rootExternalStateSerial;
  }
  [[nodiscard]] const SceneDigest &projectionDigest() const noexcept {
    return m_fence.projectionDigest;
  }
  [[nodiscard]] std::uint64_t mappingEpoch() const noexcept {
    return m_fence.mappingEpoch;
  }

  [[nodiscard]] bool contains(EditorObjectId documentId) const noexcept;
  [[nodiscard]] std::optional<std::uint32_t>
  sceneAssetIndex(EditorObjectId documentId) const noexcept;
  [[nodiscard]] std::optional<std::uint64_t>
  projectionIdentity(EditorObjectId documentId) const noexcept;
  [[nodiscard]] std::optional<std::uint64_t>
  runtimeIndex(EditorObjectId documentId) const noexcept {
    return projectionIdentity(documentId);
  }
  [[nodiscard]] std::optional<EditorObjectId>
  documentIdAt(EditorObjectKind kind,
               std::uint32_t sceneAssetIndex) const noexcept;
  [[nodiscard]] std::optional<EditorObjectId>
  documentIdForProjection(std::uint64_t projectionIdentity) const noexcept;
  [[nodiscard]] std::optional<EditorObjectId>
  documentIdForRuntime(std::uint64_t runtimeIndex) const noexcept {
    return documentIdForProjection(runtimeIndex);
  }
  [[nodiscard]] std::optional<SceneRuntimeMappingEntry>
  entry(EditorObjectId documentId) const noexcept;

  [[nodiscard]] const std::vector<SceneRuntimeMappingEntry> &
  entries() const noexcept {
    return m_entries;
  }
  [[nodiscard]] const std::vector<SceneRuntimeMappingAlias> &
  aliases() const noexcept {
    return m_aliases;
  }

  [[nodiscard]] bool
  operator==(const SceneRuntimeMapping &other) const noexcept {
    return m_fence == other.m_fence && m_entries == other.m_entries &&
           m_aliases == other.m_aliases;
  }

  [[nodiscard]] bool validate(const GltfDocument &document,
                              const SceneAssetView &asset,
                              std::string *error = nullptr) const;
  [[nodiscard]] bool validate(const GltfDocument &document,
                              const SceneAsset &asset,
                              std::string *error = nullptr) const;

  [[nodiscard]] std::string digest() const;

private:
  struct CoordinateBinding final {
    EditorObjectId documentId{};
    std::uint32_t sceneAssetIndex{kInvalidIndex};
  };

  SceneRuntimeMappingFence m_fence;
  std::vector<SceneRuntimeMappingEntry> m_entries;
  std::vector<SceneRuntimeMappingAlias> m_aliases;
  std::vector<CoordinateBinding> m_coordinateBindings;
  std::vector<std::size_t> m_projectionOrder;
};

} // namespace demo::editor
