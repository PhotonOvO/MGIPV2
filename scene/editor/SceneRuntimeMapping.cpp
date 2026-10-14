#include <mgif/scene_editor/SceneRuntimeMapping.h>
#include <mgif/scene_editor/SceneChangeCanonicalizer.h>

#if !defined(MGIF_SCENE_MAPPING_FACTORY_ONLY)
#include <mgif/scene_editor/GltfDocument.h>

#include "../SceneAsset.h"
#include "../SceneAssetView.h"
#endif

#include <algorithm>
#include <array>
#include <span>
#include <tuple>
#include <type_traits>
#include <vector>

namespace demo::editor {
namespace {

#if !defined(MGIF_SCENE_MAPPING_FACTORY_ONLY)
[[nodiscard]] std::size_t kindIndex(EditorObjectKind kind) noexcept {
  return static_cast<std::size_t>(kind);
}
#endif

#if !defined(MGIF_SCENE_MAPPING_FACTORY_ONLY)
[[nodiscard]] std::size_t assetCapacity(const SceneAssetView &asset,
                                        EditorObjectKind kind) noexcept {
  switch (kind) {
  case EditorObjectKind::node:
    return asset.nodes.size();
  case EditorObjectKind::primitive:
    return asset.primitives.size();
  case EditorObjectKind::mesh:
    return asset.meshes.size();
  case EditorObjectKind::material:
    return asset.materials.size();
  case EditorObjectKind::image:
    return asset.textures.size();
  case EditorObjectKind::camera:
    return asset.cameras.size();
  case EditorObjectKind::light:
    return asset.lights.size();
  case EditorObjectKind::animation:
    return asset.animations.size();
  case EditorObjectKind::scene:
  case EditorObjectKind::texture:
  case EditorObjectKind::sampler:
  case EditorObjectKind::accessor:
  case EditorObjectKind::bufferView:
  case EditorObjectKind::buffer:
  case EditorObjectKind::skin:
  case EditorObjectKind::invalid:
  case EditorObjectKind::count:
    return 0;
  }
  return 0;
}
#endif

[[nodiscard]] auto idKey(EditorObjectId id) noexcept {
  return std::tuple{static_cast<std::uint8_t>(id.kind), id.authority.value,
                    id.slot, id.generation};
}

[[nodiscard]] auto slotKey(EditorObjectId id) noexcept {
  return std::tuple{static_cast<std::uint8_t>(id.kind), id.authority.value,
                    id.slot};
}

[[nodiscard]] auto reverseKey(EditorObjectKind kind,
                              std::uint32_t index) noexcept {
  return std::pair{static_cast<std::uint8_t>(kind), index};
}

[[nodiscard]] auto aliasKey(const SceneRuntimeMappingAlias &alias) noexcept {
  return std::tuple{static_cast<std::uint8_t>(alias.kind),
                    alias.sceneAssetIndex, idKey(alias.documentId)};
}

template <typename T>
void appendInteger(std::vector<std::uint8_t> &bytes, T value) {
  using U = std::make_unsigned_t<T>;
  std::uint64_t bits = static_cast<std::uint64_t>(static_cast<U>(value));
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    bytes.push_back(static_cast<std::uint8_t>(bits & 0xffu));
    bits >>= 8u;
  }
}

void appendId(std::vector<std::uint8_t> &bytes, EditorObjectId id) {
  appendInteger(bytes, static_cast<std::uint8_t>(id.kind));
  appendInteger(bytes, id.authority.value);
  appendInteger(bytes, id.slot);
  appendInteger(bytes, id.generation);
}

[[nodiscard]] std::string digestHex(const SceneDigest &digest) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2u);
  for (const std::uint8_t byte : digest) {
    result.push_back(digits[byte >> 4u]);
    result.push_back(digits[byte & 0x0fu]);
  }
  return result;
}

[[nodiscard]] std::vector<std::uint8_t> canonicalMappingBytes(
    const SceneRuntimeMappingFence &fence,
    const std::vector<SceneRuntimeMappingEntry> &entries,
    const std::vector<SceneRuntimeMappingAlias> &aliases) {
  std::vector<std::uint8_t> bytes;
  constexpr std::array<std::uint8_t, 8> magic{'M', 'G', 'I', 'F',
                                              'M', 'A', 'P', '2'};
  bytes.insert(bytes.end(), magic.begin(), magic.end());
  appendInteger<std::uint32_t>(bytes, 2u);
  appendInteger(bytes, fence.authority.value);
  appendInteger(bytes, fence.appliedDocumentRevision);
  appendInteger<std::uint8_t>(bytes, fence.activeScene ? 1u : 0u);
  if (fence.activeScene) {
    appendId(bytes, editorObjectId(*fence.activeScene));
  }
  appendInteger(bytes, fence.sceneActivationEpoch);
  appendInteger(bytes, fence.rootExternalStateSerial);
  bytes.insert(bytes.end(), fence.projectionDigest.begin(),
               fence.projectionDigest.end());
  appendInteger(bytes, fence.mappingEpoch);
  appendInteger<std::uint64_t>(bytes, entries.size());
  for (const SceneRuntimeMappingEntry &entry : entries) {
    appendId(bytes, entry.documentId);
    appendInteger(bytes, entry.sceneAssetIndex);
    appendInteger(bytes, entry.projectionIdentity);
    appendInteger(bytes, entry.runtimeIndex);
  }
  appendInteger<std::uint64_t>(bytes, aliases.size());
  for (const SceneRuntimeMappingAlias &alias : aliases) {
    appendId(bytes, alias.documentId);
    appendInteger(bytes, static_cast<std::uint8_t>(alias.kind));
    appendInteger(bytes, alias.sceneAssetIndex);
  }
  return bytes;
}

void setError(std::string *error, const char *message) {
  if (error != nullptr) {
    *error = message;
  }
}

#if !defined(MGIF_SCENE_MAPPING_FACTORY_ONLY)
[[nodiscard]] std::optional<StableSceneId>
stableActive(std::optional<SceneId> scene) noexcept {
  return scene ? std::optional<StableSceneId>{stableSceneId(scene->untyped())}
               : std::nullopt;
}

[[nodiscard]] bool validateProjectedCoordinate(
    const GltfDocument &document, const SceneAssetView &asset,
    EditorObjectId documentId, std::uint32_t sceneAssetIndex,
    std::string *error) {
  if (sceneAssetIndex >= assetCapacity(asset, documentId.kind)) {
    setError(error, "mapping SceneAsset coordinate is out of bounds");
    return false;
  }
  const auto sourceIndex = document.indexOf(documentId);
  if (!sourceIndex) {
    setError(error, "mapping stable ID has no document authority index");
    return false;
  }
  const std::uint64_t expectedIdentity =
      SceneRuntimeMapping::deriveProjectionIdentity(documentId);
  switch (documentId.kind) {
  case EditorObjectKind::node: {
    const SceneNode &node = asset.nodes[sceneAssetIndex];
    if (node.sourceNodeIndex != *sourceIndex) {
      setError(error, "node mapping source identity does not match");
      return false;
    }
    if (node.runtimeIdentity != expectedIdentity) {
      setError(error, "node runtime identity does not match its stable ID");
      return false;
    }
    return true;
  }
  case EditorObjectKind::primitive: {
    const auto typed = PrimitiveId::from(documentId);
    const auto address = typed ? document.primitiveAddress(*typed)
                               : std::nullopt;
    const ScenePrimitive &primitive = asset.primitives[sceneAssetIndex];
    if (!address || primitive.documentMeshIndex != address->meshIndex ||
        primitive.documentPrimitiveIndex != address->primitiveIndex) {
      setError(error, "primitive mapping source identity does not match");
      return false;
    }
    if (primitive.runtimeIdentity != expectedIdentity) {
      setError(error,
               "primitive runtime identity does not match its stable ID");
      return false;
    }
    return true;
  }
  case EditorObjectKind::mesh: {
    const SceneMesh &mesh = asset.meshes[sceneAssetIndex];
    if (mesh.sourceMeshIndex != *sourceIndex) {
      setError(error, "mesh mapping source identity does not match");
      return false;
    }
    if (mesh.runtimeIdentity != expectedIdentity) {
      setError(error, "mesh runtime identity does not match its stable ID");
      return false;
    }
    return true;
  }
  case EditorObjectKind::material:
  case EditorObjectKind::animation:
    if (sceneAssetIndex != *sourceIndex) {
      setError(error, "ordered projection coordinate does not match");
      return false;
    }
    return true;
  case EditorObjectKind::image:
    if (asset.textures[sceneAssetIndex].sourceImageIndex != *sourceIndex) {
      setError(error, "image projection source identity does not match");
      return false;
    }
    return true;
  case EditorObjectKind::camera: {
    const SceneCamera &camera = asset.cameras[sceneAssetIndex];
    if (camera.sourceCameraIndex != *sourceIndex) {
      setError(error, "camera mapping source identity does not match");
      return false;
    }
    if (camera.runtimeIdentity != expectedIdentity) {
      setError(error, "camera runtime identity does not match its stable ID");
      return false;
    }
    return true;
  }
  case EditorObjectKind::light: {
    const SceneLight &light = asset.lights[sceneAssetIndex];
    if (light.sourceLightIndex != *sourceIndex) {
      setError(error, "light mapping source identity does not match");
      return false;
    }
    if (light.runtimeIdentity != expectedIdentity) {
      setError(error, "light runtime identity does not match its stable ID");
      return false;
    }
    return true;
  }
  case EditorObjectKind::scene:
  case EditorObjectKind::texture:
  case EditorObjectKind::sampler:
  case EditorObjectKind::accessor:
  case EditorObjectKind::bufferView:
  case EditorObjectKind::buffer:
  case EditorObjectKind::skin:
  case EditorObjectKind::invalid:
  case EditorObjectKind::count:
    setError(error, "mapping kind has no renderer-facing coordinate");
    return false;
  }
  setError(error, "mapping kind is invalid");
  return false;
}
#endif

} // namespace

std::optional<SceneRuntimeMapping> SceneRuntimeMapping::create(
    SceneRuntimeMappingFence fence,
    std::vector<SceneRuntimeMappingEntry> entries, std::string *error) {
  return create(std::move(fence), std::move(entries),
                std::vector<SceneRuntimeMappingAlias>{}, error);
}

std::optional<SceneRuntimeMapping> SceneRuntimeMapping::create(
    SceneRuntimeMappingFence fence,
    std::vector<SceneRuntimeMappingEntry> entries,
    std::vector<SceneRuntimeMappingAlias> aliases, std::string *error) {
  if (!fence.authority.valid() || !sceneDigestPresent(fence.projectionDigest) ||
      fence.mappingEpoch == 0u ||
      (fence.activeScene &&
       (!fence.activeScene->valid() ||
        fence.activeScene->authority != fence.authority ||
        fence.activeScene->kind != EditorObjectKind::scene))) {
    setError(error, "mapping fence is incomplete or invalid");
    return std::nullopt;
  }
  for (SceneRuntimeMappingEntry &item : entries) {
    if (!item.documentId.valid() ||
        item.documentId.authority != fence.authority) {
      setError(error, "mapping entry has an invalid or foreign stable ID");
      return std::nullopt;
    }
    if (item.sceneAssetIndex == kInvalidIndex) {
      if (item.projectionIdentity != kInvalidProjectionIdentity ||
          item.runtimeIndex != kInvalidRuntimeIndex) {
        setError(error,
                 "unprojected mapping entry has a projection identity");
        return std::nullopt;
      }
      continue;
    }
    if (item.projectionIdentity == kInvalidProjectionIdentity &&
        item.runtimeIndex != kInvalidRuntimeIndex) {
      item.projectionIdentity = item.runtimeIndex;
    }
    if (item.runtimeIndex == kInvalidRuntimeIndex &&
        item.projectionIdentity != kInvalidProjectionIdentity) {
      item.runtimeIndex = item.projectionIdentity;
    }
    if (item.projectionIdentity == 0u ||
        item.projectionIdentity == kInvalidProjectionIdentity ||
        item.runtimeIndex != item.projectionIdentity) {
      setError(error,
               "projected mapping entry has an invalid projection identity");
      return std::nullopt;
    }
  }

  std::sort(entries.begin(), entries.end(), [](const auto &left,
                                                const auto &right) {
    return idKey(left.documentId) < idKey(right.documentId);
  });
  for (std::size_t index = 1; index < entries.size(); ++index) {
    if (entries[index - 1u].documentId == entries[index].documentId) {
      setError(error, "mapping contains a duplicate stable ID");
      return std::nullopt;
    }
    if (slotKey(entries[index - 1u].documentId) ==
        slotKey(entries[index].documentId)) {
      setError(error,
               "mapping contains two generations of the same stable slot");
      return std::nullopt;
    }
  }

  std::vector<std::size_t> projectionOrder;
  projectionOrder.reserve(entries.size());
  for (std::size_t index = 0; index < entries.size(); ++index) {
    if (entries[index].sceneAssetIndex != kInvalidIndex) {
      projectionOrder.push_back(index);
    }
  }
  std::sort(projectionOrder.begin(), projectionOrder.end(),
            [&](std::size_t left, std::size_t right) {
              return entries[left].projectionIdentity <
                     entries[right].projectionIdentity;
            });
  for (std::size_t index = 1; index < projectionOrder.size(); ++index) {
    if (entries[projectionOrder[index - 1u]].projectionIdentity ==
        entries[projectionOrder[index]].projectionIdentity) {
      setError(error, "mapping contains a duplicate projection identity");
      return std::nullopt;
    }
  }

  for (SceneRuntimeMappingAlias &alias : aliases) {
    if (!alias.documentId.valid() ||
        alias.documentId.authority != fence.authority) {
      setError(error, "mapping alias has an invalid or foreign stable ID");
      return std::nullopt;
    }
    if (alias.kind != alias.documentId.kind) {
      setError(error, "mapping alias kind does not match its stable ID");
      return std::nullopt;
    }
    if (alias.sceneAssetIndex == kInvalidIndex) {
      setError(error, "mapping alias has an invalid SceneAsset coordinate");
      return std::nullopt;
    }
    const auto primary = std::lower_bound(
        entries.begin(), entries.end(), alias.documentId,
        [](const SceneRuntimeMappingEntry &entry, EditorObjectId value) {
          return idKey(entry.documentId) < idKey(value);
        });
    if (primary == entries.end() || primary->documentId != alias.documentId ||
        primary->sceneAssetIndex == kInvalidIndex) {
      setError(error, "mapping alias has no projected primary entry");
      return std::nullopt;
    }
    if (alias.sceneAssetIndex < primary->sceneAssetIndex) {
      std::swap(alias.sceneAssetIndex, primary->sceneAssetIndex);
    }
  }
  std::sort(aliases.begin(), aliases.end(),
            [](const SceneRuntimeMappingAlias &left,
               const SceneRuntimeMappingAlias &right) {
              return aliasKey(left) < aliasKey(right);
            });

  std::vector<CoordinateBinding> coordinateBindings;
  coordinateBindings.reserve(projectionOrder.size() + aliases.size());
  for (const std::size_t entryIndex : projectionOrder) {
    const SceneRuntimeMappingEntry &entry = entries[entryIndex];
    coordinateBindings.push_back(
        CoordinateBinding{entry.documentId, entry.sceneAssetIndex});
  }
  for (const SceneRuntimeMappingAlias &alias : aliases) {
    coordinateBindings.push_back(
        CoordinateBinding{alias.documentId, alias.sceneAssetIndex});
  }
  std::sort(coordinateBindings.begin(), coordinateBindings.end(),
            [](const CoordinateBinding &left,
               const CoordinateBinding &right) {
              const auto leftKey =
                  reverseKey(left.documentId.kind, left.sceneAssetIndex);
              const auto rightKey =
                  reverseKey(right.documentId.kind, right.sceneAssetIndex);
              return leftKey != rightKey
                         ? leftKey < rightKey
                         : idKey(left.documentId) < idKey(right.documentId);
            });
  for (std::size_t index = 1; index < coordinateBindings.size(); ++index) {
    const CoordinateBinding &left = coordinateBindings[index - 1u];
    const CoordinateBinding &right = coordinateBindings[index];
    if (reverseKey(left.documentId.kind, left.sceneAssetIndex) ==
        reverseKey(right.documentId.kind, right.sceneAssetIndex)) {
      setError(error, left.documentId == right.documentId
                          ? "mapping repeats a SceneAsset coordinate"
                          : "mapping coordinate has conflicting stable-ID owners");
      return std::nullopt;
    }
  }

  SceneRuntimeMapping result;
  result.m_fence = std::move(fence);
  result.m_entries = std::move(entries);
  result.m_aliases = std::move(aliases);
  result.m_coordinateBindings = std::move(coordinateBindings);
  result.m_projectionOrder = std::move(projectionOrder);
  if (error != nullptr) {
    error->clear();
  }
  return result;
}

std::uint64_t SceneRuntimeMapping::deriveProjectionIdentity(
    EditorObjectId documentId) noexcept {
  if (!documentId.valid()) {
    return kInvalidProjectionIdentity;
  }
  std::uint64_t value = 14695981039346656037ull;
  const auto mix = [&value](std::uint64_t part) {
    for (unsigned int shift = 0; shift < 64u; shift += 8u) {
      value ^= (part >> shift) & 0xffu;
      value *= 1099511628211ull;
    }
  };
  mix(static_cast<std::uint64_t>(documentId.kind));
  mix(documentId.authority.value);
  mix(documentId.slot);
  mix(documentId.generation);
  if (value == 0u) {
    return 1u;
  }
  return value == kInvalidProjectionIdentity ? value - 1u : value;
}

bool SceneRuntimeMapping::contains(EditorObjectId documentId) const noexcept {
  return entry(documentId).has_value();
}

std::optional<std::uint32_t>
SceneRuntimeMapping::sceneAssetIndex(EditorObjectId documentId) const noexcept {
  const auto found = entry(documentId);
  return found && found->sceneAssetIndex != kInvalidIndex
             ? std::optional<std::uint32_t>(found->sceneAssetIndex)
             : std::nullopt;
}

std::optional<std::uint64_t> SceneRuntimeMapping::projectionIdentity(
    EditorObjectId documentId) const noexcept {
  const auto found = entry(documentId);
  return found && found->projectionIdentity != kInvalidProjectionIdentity
             ? std::optional<std::uint64_t>(found->projectionIdentity)
             : std::nullopt;
}

std::optional<EditorObjectId> SceneRuntimeMapping::documentIdAt(
    EditorObjectKind kind, std::uint32_t sceneAssetIndex) const noexcept {
  const auto key = reverseKey(kind, sceneAssetIndex);
  const auto found = std::lower_bound(
      m_coordinateBindings.begin(), m_coordinateBindings.end(), key,
      [](const CoordinateBinding &binding, const auto &value) {
        return reverseKey(binding.documentId.kind, binding.sceneAssetIndex) <
               value;
      });
  if (found != m_coordinateBindings.end() &&
      reverseKey(found->documentId.kind, found->sceneAssetIndex) == key) {
    return found->documentId;
  }
  return std::nullopt;
}

std::optional<EditorObjectId> SceneRuntimeMapping::documentIdForProjection(
    std::uint64_t projectionIdentity) const noexcept {
  if (projectionIdentity == 0u ||
      projectionIdentity == kInvalidProjectionIdentity) {
    return std::nullopt;
  }
  const auto found = std::lower_bound(
      m_projectionOrder.begin(), m_projectionOrder.end(), projectionIdentity,
      [&](std::size_t entryIndex, std::uint64_t value) {
        return m_entries[entryIndex].projectionIdentity < value;
      });
  return found != m_projectionOrder.end() &&
                 m_entries[*found].projectionIdentity == projectionIdentity
             ? std::optional<EditorObjectId>(m_entries[*found].documentId)
             : std::nullopt;
}

std::optional<SceneRuntimeMappingEntry>
SceneRuntimeMapping::entry(EditorObjectId documentId) const noexcept {
  if (!documentId.valid()) {
    return std::nullopt;
  }
  const auto found = std::lower_bound(
      m_entries.begin(), m_entries.end(), documentId,
      [](const SceneRuntimeMappingEntry &item, EditorObjectId value) {
        return idKey(item.documentId) < idKey(value);
      });
  return found != m_entries.end() && found->documentId == documentId
             ? std::optional<SceneRuntimeMappingEntry>(*found)
             : std::nullopt;
}

#if !defined(MGIF_SCENE_MAPPING_FACTORY_ONLY)
bool SceneRuntimeMapping::validate(const GltfDocument &document,
                                   const SceneAsset &asset,
                                   std::string *error) const {
  return validate(document, makeSceneAssetView(asset), error);
}

bool SceneRuntimeMapping::validate(const GltfDocument &document,
                                   const SceneAssetView &asset,
                                   std::string *error) const {
  const std::optional<SceneId> active = document.activeSceneId();
  const std::uint64_t rootSerial = active
                                       ? document.sceneRootTransformState(*active)
                                             .externalStateSerial
                                       : 0u;
  if (m_fence.authority != document.documentAuthority() ||
      m_fence.appliedDocumentRevision != document.revision() ||
      m_fence.activeScene != stableActive(active) ||
      m_fence.sceneActivationEpoch != document.sceneActivationEpoch() ||
      m_fence.rootExternalStateSerial != rootSerial ||
      !sceneDigestPresent(m_fence.projectionDigest) ||
      m_fence.mappingEpoch == 0u) {
    setError(error, "mapping fence does not match the document projection");
    return false;
  }

  std::size_t expectedEntries = 0;
  for (std::size_t kind = 1; kind < kindIndex(EditorObjectKind::count); ++kind) {
    expectedEntries +=
        document.objectCount(static_cast<EditorObjectKind>(kind));
  }
  if (m_entries.size() != expectedEntries) {
    setError(error, "mapping does not contain every live typed document ID");
    return false;
  }
  std::array<std::size_t, static_cast<std::size_t>(EditorObjectKind::count)>
      kindCounts{};
  for (const SceneRuntimeMappingEntry &item : m_entries) {
    if (!document.contains(item.documentId)) {
      setError(error, "mapping contains a stale or foreign document ID");
      return false;
    }
    ++kindCounts[kindIndex(item.documentId.kind)];
    if (item.sceneAssetIndex == kInvalidIndex) {
      continue;
    }
    const std::uint64_t expectedIdentity =
        deriveProjectionIdentity(item.documentId);
    if (item.projectionIdentity != expectedIdentity ||
        item.runtimeIndex != expectedIdentity) {
      setError(error,
               "mapping projection identity is not derived from its stable ID");
      return false;
    }
    if (!validateProjectedCoordinate(document, asset, item.documentId,
                                     item.sceneAssetIndex, error)) {
      return false;
    }
  }
  for (const SceneRuntimeMappingAlias &alias : m_aliases) {
    if (alias.kind != alias.documentId.kind ||
        !document.contains(alias.documentId)) {
      setError(error, "mapping alias contains stale, foreign, or mismatched data");
      return false;
    }
    const auto primary = entry(alias.documentId);
    if (!primary || primary->sceneAssetIndex == kInvalidIndex ||
        primary->projectionIdentity !=
            deriveProjectionIdentity(alias.documentId)) {
      setError(error, "mapping alias has no valid projected primary entry");
      return false;
    }
    if (!validateProjectedCoordinate(document, asset, alias.documentId,
                                     alias.sceneAssetIndex, error)) {
      return false;
    }
  }
  for (std::size_t kind = 1; kind < kindIndex(EditorObjectKind::count); ++kind) {
    if (kindCounts[kind] !=
        document.objectCount(static_cast<EditorObjectKind>(kind))) {
      setError(error, "mapping kind cardinality does not match the document");
      return false;
    }
  }
  const auto requireEveryCoordinate = [&](EditorObjectKind kind,
                                          std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
      const auto owner =
          documentIdAt(kind, static_cast<std::uint32_t>(index));
      if (!owner) {
        setError(error,
                 "renderer-facing SceneAsset coordinate has no stable owner");
        return false;
      }
      if (!validateProjectedCoordinate(document, asset, *owner,
                                       static_cast<std::uint32_t>(index),
                                       error)) {
        return false;
      }
    }
    return true;
  };
  if (!requireEveryCoordinate(EditorObjectKind::node, asset.nodes.size()) ||
      !requireEveryCoordinate(EditorObjectKind::primitive,
                              asset.primitives.size()) ||
      !requireEveryCoordinate(EditorObjectKind::mesh, asset.meshes.size()) ||
      !requireEveryCoordinate(EditorObjectKind::material,
                              asset.materials.size()) ||
      !requireEveryCoordinate(EditorObjectKind::image, asset.textures.size()) ||
      !requireEveryCoordinate(EditorObjectKind::camera, asset.cameras.size()) ||
      !requireEveryCoordinate(EditorObjectKind::light, asset.lights.size()) ||
      !requireEveryCoordinate(EditorObjectKind::animation,
                              asset.animations.size())) {
    return false;
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}
#endif

std::string SceneRuntimeMapping::digest() const {
  const std::vector<std::uint8_t> bytes =
      canonicalMappingBytes(m_fence, m_entries, m_aliases);
  return digestHex(SceneChangeCanonicalizer::computeSha256(bytes));
}

} // namespace demo::editor
