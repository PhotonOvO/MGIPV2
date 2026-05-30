#include "SceneAssetSerializer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <utility>
#include <sstream>
#include <type_traits>

namespace demo {

namespace {

constexpr char     kAssetMagic[8] = {'S', 'A', 'S', 'S', 'E', 'T', 0, 0};
constexpr uint32_t kAssetVersion  = SceneAssetSerializer::kCurrentVersion;
constexpr uint64_t kMaxReasonableStringBytes = 1ull << 20;
constexpr uint64_t kMaxReasonableVectorElements = 1ull << 22;
constexpr uint32_t kMaxReasonableMeshCount = 1u << 16;
constexpr uint32_t kMaxReasonableMaterialCount = 1u << 14;
constexpr uint32_t kMaxReasonableTextureCount = 1u << 14;
constexpr uint32_t kMaxReasonableNodeCount = 1u << 16;
constexpr uint32_t kMaxReasonableDependencyCount = 1u << 14;
constexpr uint64_t kMaxReasonablePayloadBytes = 8ull << 30;

struct AssetHeader {
  char     magic[8]{};
  uint32_t version{0};
  uint32_t reserved{0};
  uint64_t sourceFileSize{0};
  int64_t  sourceWriteTimeTicks{0};
  uint64_t sourcePathHash{0};
  uint32_t meshCount{0};
  uint32_t materialCount{0};
  uint32_t textureCount{0};
  uint32_t nodeCount{0};
  uint32_t rootNodeCount{0};
  uint32_t dependencyCount{0};
  uint32_t reserved1{0};
  uint64_t vertexPayloadBytes{0};
  uint64_t indexPayloadBytes{0};
  uint64_t texturePayloadBytes{0};
};

struct SectionTable {
  uint64_t meshesOffset{0};
  uint64_t meshesSize{0};
  uint64_t materialsOffset{0};
  uint64_t materialsSize{0};
  uint64_t texturesOffset{0};
  uint64_t texturesSize{0};
  uint64_t nodesOffset{0};
  uint64_t nodesSize{0};
  uint64_t rootNodesOffset{0};
  uint64_t rootNodesSize{0};
  uint64_t dependenciesOffset{0};
  uint64_t dependenciesSize{0};
  uint64_t vertexPayloadOffset{0};
  uint64_t vertexPayloadSize{0};
  uint64_t indexPayloadOffset{0};
  uint64_t indexPayloadSize{0};
  uint64_t texturePayloadOffset{0};
  uint64_t texturePayloadSize{0};
};

[[nodiscard]] bool hasReasonableCounts(const AssetHeader& h) {
  return h.meshCount <= kMaxReasonableMeshCount
      && h.materialCount <= kMaxReasonableMaterialCount
      && h.textureCount <= kMaxReasonableTextureCount
      && h.nodeCount <= kMaxReasonableNodeCount
      && h.dependencyCount <= kMaxReasonableDependencyCount
      && h.vertexPayloadBytes <= kMaxReasonablePayloadBytes
      && h.indexPayloadBytes <= kMaxReasonablePayloadBytes
      && h.texturePayloadBytes <= kMaxReasonablePayloadBytes;
}

[[nodiscard]] bool sectionFits(uint64_t offset, uint64_t size, uint64_t fileSize) {
  return offset <= fileSize && size <= fileSize - offset;
}

[[nodiscard]] bool hasValidSectionTable(const AssetHeader& h, const SectionTable& table, uint64_t fileSize) {
  const uint64_t minimumSize = sizeof(AssetHeader) + sizeof(SectionTable);
  if(fileSize < minimumSize) {
    return false;
  }

  if(table.meshesSize != static_cast<uint64_t>(h.meshCount) * sizeof(SceneMesh)
     || table.rootNodesSize != static_cast<uint64_t>(h.rootNodeCount) * sizeof(uint32_t)
     || table.vertexPayloadSize != h.vertexPayloadBytes
     || table.indexPayloadSize != h.indexPayloadBytes
     || table.texturePayloadSize != h.texturePayloadBytes) {
    return false;
  }

  const std::array<std::pair<uint64_t, uint64_t>, 9> sections{{
      {table.meshesOffset, table.meshesSize},
      {table.materialsOffset, table.materialsSize},
      {table.texturesOffset, table.texturesSize},
      {table.nodesOffset, table.nodesSize},
      {table.rootNodesOffset, table.rootNodesSize},
      {table.dependenciesOffset, table.dependenciesSize},
      {table.vertexPayloadOffset, table.vertexPayloadSize},
      {table.indexPayloadOffset, table.indexPayloadSize},
      {table.texturePayloadOffset, table.texturePayloadSize},
  }};

  uint64_t expectedOffset = minimumSize;
  for(const auto& [offset, size] : sections) {
    if(offset != expectedOffset || !sectionFits(offset, size, fileSize)) {
      return false;
    }
    expectedOffset += size;
  }

  return expectedOffset == fileSize;
}

[[nodiscard]] uint64_t hashPath(const std::filesystem::path& path) {
  return static_cast<uint64_t>(std::hash<std::string>{}(std::filesystem::weakly_canonical(path).generic_string()));
}

[[nodiscard]] int64_t fileWriteTicks(const std::filesystem::path& path) {
  return static_cast<int64_t>(std::filesystem::last_write_time(path).time_since_epoch().count());
}

[[nodiscard]] bool dependencyMatchesSource(const SceneDependency& dependency,
                                           const std::filesystem::path& sourceDirectory) {
  if(dependency.relativePath.empty()) {
    return true;
  }

  const std::filesystem::path dependencyPath = sourceDirectory / std::filesystem::path(dependency.relativePath);
  if(!std::filesystem::exists(dependencyPath) || !std::filesystem::is_regular_file(dependencyPath)) {
    return false;
  }

  return std::filesystem::file_size(dependencyPath) == dependency.fileSize
      && fileWriteTicks(dependencyPath) == dependency.writeTimeTicks
      && hashPath(dependencyPath) == dependency.pathHash;
}

template <typename T>
void writePod(std::ostream& stream, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>, "writePod requires trivially copyable types");
  stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool readPod(std::istream& stream, T& value) {
  static_assert(std::is_trivially_copyable_v<T>, "readPod requires trivially copyable types");
  stream.read(reinterpret_cast<char*>(&value), sizeof(T));
  return static_cast<bool>(stream);
}

void writeString(std::ostream& stream, const std::string& value) {
  const uint64_t size = static_cast<uint64_t>(value.size());
  writePod(stream, size);
  if (size > 0) {
    stream.write(value.data(), static_cast<std::streamsize>(size));
  }
}

bool readString(std::istream& stream, std::string& value) {
  uint64_t size = 0;
  if (!readPod(stream, size)) return false;
  if (size > kMaxReasonableStringBytes) return false;
  value.resize(static_cast<size_t>(size));
  if (size > 0) {
    stream.read(value.data(), static_cast<std::streamsize>(size));
  }
  return static_cast<bool>(stream);
}

template <typename T>
void writeVector(std::ostream& stream, const std::vector<T>& values) {
  static_assert(std::is_trivially_copyable_v<T>, "writeVector requires trivially copyable element types");
  const uint64_t size = static_cast<uint64_t>(values.size());
  writePod(stream, size);
  if (size > 0) {
    stream.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(sizeof(T) * size));
  }
}

template <typename T>
bool readVector(std::istream& stream, std::vector<T>& values) {
  static_assert(std::is_trivially_copyable_v<T>, "readVector requires trivially copyable element types");
  uint64_t size = 0;
  if (!readPod(stream, size)) return false;
  if (size > kMaxReasonableVectorElements) return false;
  values.resize(static_cast<size_t>(size));
  if (size > 0) {
    stream.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(sizeof(T) * size));
  }
  return static_cast<bool>(stream);
}

// SceneMesh is fully POD — direct array write
static_assert(std::is_trivially_copyable_v<SceneMesh>, "SceneMesh must be POD for serialization");

void writeMaterial(std::ostream& stream, const SceneMaterial& mat) {
  writePod(stream, mat.baseColorFactor);
  writePod(stream, mat.metallicFactor);
  writePod(stream, mat.roughnessFactor);
  writePod(stream, mat.normalScale);
  writePod(stream, mat.occlusionStrength);
  writePod(stream, mat.emissiveFactor);
  writePod(stream, mat._padding);
  writePod(stream, mat.baseColorTexture);
  writePod(stream, mat.normalTexture);
  writePod(stream, mat.metallicRoughnessTexture);
  writePod(stream, mat.occlusionTexture);
  writePod(stream, mat.emissiveTexture);
  writePod(stream, mat.alphaMode);
  writePod(stream, mat.alphaCutoff);
  writePod(stream, mat.materialWorkflow);
  writePod(stream, mat.doubleSided);
  writeString(stream, mat.name);
}

bool readMaterial(std::istream& stream, SceneMaterial& mat) {
  return readPod(stream, mat.baseColorFactor)
      && readPod(stream, mat.metallicFactor)
      && readPod(stream, mat.roughnessFactor)
      && readPod(stream, mat.normalScale)
      && readPod(stream, mat.occlusionStrength)
      && readPod(stream, mat.emissiveFactor)
      && readPod(stream, mat._padding)
      && readPod(stream, mat.baseColorTexture)
      && readPod(stream, mat.normalTexture)
      && readPod(stream, mat.metallicRoughnessTexture)
      && readPod(stream, mat.occlusionTexture)
      && readPod(stream, mat.emissiveTexture)
      && readPod(stream, mat.alphaMode)
      && readPod(stream, mat.alphaCutoff)
      && readPod(stream, mat.materialWorkflow)
      && readPod(stream, mat.doubleSided)
      && readString(stream, mat.name);
}

// SceneTexture is mostly POD except uri
void writeTexture(std::ostream& stream, const SceneTexture& tex) {
  writePod(stream, tex.width);
  writePod(stream, tex.height);
  writePod(stream, tex.mipLevels);
  writePod(stream, tex.format);
  writePod(stream, tex.payloadOffset);
  writePod(stream, tex.payloadSize);
  writePod(stream, tex.isKtx2);
  writeString(stream, tex.uri);
}

bool readTexture(std::istream& stream, SceneTexture& tex) {
  return readPod(stream, tex.width)
      && readPod(stream, tex.height)
      && readPod(stream, tex.mipLevels)
      && readPod(stream, tex.format)
      && readPod(stream, tex.payloadOffset)
      && readPod(stream, tex.payloadSize)
      && readPod(stream, tex.isKtx2)
      && readString(stream, tex.uri);
}

void writeDependency(std::ostream& stream, const SceneDependency& dependency) {
  writeString(stream, dependency.relativePath);
  writePod(stream, dependency.fileSize);
  writePod(stream, dependency.writeTimeTicks);
  writePod(stream, dependency.pathHash);
}

bool readDependency(std::istream& stream, SceneDependency& dependency) {
  return readString(stream, dependency.relativePath)
      && readPod(stream, dependency.fileSize)
      && readPod(stream, dependency.writeTimeTicks)
      && readPod(stream, dependency.pathHash);
}

void writeNode(std::ostream& stream, const SceneNode& node) {
  writeString(stream, node.name);
  writePod(stream, node.parent);
  writeVector(stream, node.children);
  writeVector(stream, node.meshRefs);
  writePod(stream, node.translation);
  writePod(stream, node.rotation);
  writePod(stream, node.scale);
  writePod(stream, node.localTransform);
  writePod(stream, node.worldTransform);
}

bool readNode(std::istream& stream, SceneNode& node) {
  return readString(stream, node.name)
      && readPod(stream, node.parent)
      && readVector(stream, node.children)
      && readVector(stream, node.meshRefs)
      && readPod(stream, node.translation)
      && readPod(stream, node.rotation)
      && readPod(stream, node.scale)
      && readPod(stream, node.localTransform)
      && readPod(stream, node.worldTransform);
}

} // namespace

std::filesystem::path SceneAssetSerializer::buildAssetPath(const std::filesystem::path& sourcePath) {
  return sourcePath.parent_path() / "Cache" / (sourcePath.filename().string() + ".sceneasset");
}

bool SceneAssetSerializer::save(const std::filesystem::path& assetPath,
                                const SceneAsset&            asset,
                                const std::filesystem::path& sourcePath) {
  m_lastError.clear();

  try {
    std::filesystem::create_directories(assetPath.parent_path());
  } catch (const std::exception& e) {
    m_lastError = e.what();
    return false;
  }

  // Pre-serialize variable-length sections to compute sizes
  std::string materialsData;
  std::string texturesData;
  std::string nodesData;
  std::string dependenciesData;
  {
    std::ostringstream matStream(std::ios::binary);
    for (const auto& mat : asset.materials) {
      writeMaterial(matStream, mat);
    }
    materialsData = matStream.str();
  }
  {
    std::ostringstream texStream(std::ios::binary);
    for (const auto& tex : asset.textures) {
      writeTexture(texStream, tex);
    }
    texturesData = texStream.str();
  }
  {
    std::ostringstream nodeStream(std::ios::binary);
    for (const auto& node : asset.nodes) {
      writeNode(nodeStream, node);
    }
    nodesData = nodeStream.str();
  }
  {
    std::ostringstream dependencyStream(std::ios::binary);
    for(const SceneDependency& dependency : asset.dependencies) {
      writeDependency(dependencyStream, dependency);
    }
    dependenciesData = dependencyStream.str();
  }

  AssetHeader header{};
  std::memcpy(header.magic, kAssetMagic, sizeof(kAssetMagic));
  header.version              = kAssetVersion;
  header.sourceFileSize       = std::filesystem::exists(sourcePath) ? std::filesystem::file_size(sourcePath) : 0;
  header.sourceWriteTimeTicks = std::filesystem::exists(sourcePath) ? fileWriteTicks(sourcePath) : 0;
  header.sourcePathHash       = hashPath(sourcePath);
  header.meshCount            = static_cast<uint32_t>(asset.meshes.size());
  header.materialCount        = static_cast<uint32_t>(asset.materials.size());
  header.textureCount         = static_cast<uint32_t>(asset.textures.size());
  header.nodeCount            = static_cast<uint32_t>(asset.nodes.size());
  header.rootNodeCount        = static_cast<uint32_t>(asset.rootNodes.size());
  header.dependencyCount      = static_cast<uint32_t>(asset.dependencies.size());
  header.vertexPayloadBytes   = static_cast<uint64_t>(asset.vertexPayload.size());
  header.indexPayloadBytes    = static_cast<uint64_t>(asset.indexPayload.size());
  header.texturePayloadBytes  = static_cast<uint64_t>(asset.texturePayload.size());

  const uint64_t headerSize = sizeof(AssetHeader);
  const uint64_t tableSize  = sizeof(SectionTable);

  SectionTable table{};
  uint64_t currentOffset = headerSize + tableSize;

  table.meshesOffset = currentOffset;
  table.meshesSize   = static_cast<uint64_t>(asset.meshes.size()) * sizeof(SceneMesh);
  currentOffset += table.meshesSize;

  table.materialsOffset = currentOffset;
  table.materialsSize   = materialsData.size();
  currentOffset += table.materialsSize;

  table.texturesOffset = currentOffset;
  table.texturesSize   = texturesData.size();
  currentOffset += table.texturesSize;

  table.nodesOffset = currentOffset;
  table.nodesSize   = nodesData.size();
  currentOffset += table.nodesSize;

  table.rootNodesOffset = currentOffset;
  table.rootNodesSize   = static_cast<uint64_t>(asset.rootNodes.size()) * sizeof(uint32_t);
  currentOffset += table.rootNodesSize;

  table.dependenciesOffset = currentOffset;
  table.dependenciesSize   = dependenciesData.size();
  currentOffset += table.dependenciesSize;

  table.vertexPayloadOffset = currentOffset;
  table.vertexPayloadSize   = header.vertexPayloadBytes;
  currentOffset += table.vertexPayloadSize;

  table.indexPayloadOffset = currentOffset;
  table.indexPayloadSize   = header.indexPayloadBytes;
  currentOffset += table.indexPayloadSize;

  table.texturePayloadOffset = currentOffset;
  table.texturePayloadSize   = header.texturePayloadBytes;
  currentOffset += table.texturePayloadSize;

  std::ofstream stream(assetPath, std::ios::binary | std::ios::trunc);
  if (!stream) {
    m_lastError = "Failed to open asset file for writing";
    return false;
  }

  // Write header
  writePod(stream, header);
  // Write section table (immediately after header)
  writePod(stream, table);

  // Write meshes
  if (!asset.meshes.empty()) {
    stream.write(reinterpret_cast<const char*>(asset.meshes.data()), static_cast<std::streamsize>(table.meshesSize));
  }

  // Write materials
  if (!materialsData.empty()) {
    stream.write(materialsData.data(), static_cast<std::streamsize>(materialsData.size()));
  }

  // Write textures
  if (!texturesData.empty()) {
    stream.write(texturesData.data(), static_cast<std::streamsize>(texturesData.size()));
  }

  // Write nodes
  if (!nodesData.empty()) {
    stream.write(nodesData.data(), static_cast<std::streamsize>(nodesData.size()));
  }

  // Write root nodes
  if (!asset.rootNodes.empty()) {
    stream.write(reinterpret_cast<const char*>(asset.rootNodes.data()), static_cast<std::streamsize>(table.rootNodesSize));
  }

  // Write dependency metadata
  if(!dependenciesData.empty()) {
    stream.write(dependenciesData.data(), static_cast<std::streamsize>(dependenciesData.size()));
  }

  // Write payloads
  if (!asset.vertexPayload.empty()) {
    stream.write(reinterpret_cast<const char*>(asset.vertexPayload.data()), static_cast<std::streamsize>(asset.vertexPayload.size()));
  }
  if (!asset.indexPayload.empty()) {
    stream.write(reinterpret_cast<const char*>(asset.indexPayload.data()), static_cast<std::streamsize>(asset.indexPayload.size()));
  }
  if (!asset.texturePayload.empty()) {
    stream.write(reinterpret_cast<const char*>(asset.texturePayload.data()), static_cast<std::streamsize>(asset.texturePayload.size()));
  }

  if(!stream) {
    m_lastError = "Failed while writing asset file";
    return false;
  }

  return true;
}

bool SceneAssetSerializer::load(const std::filesystem::path& assetPath, SceneAsset& asset) {
  m_lastError.clear();

  try {
    std::ifstream stream(assetPath, std::ios::binary);
    if (!stream) {
      m_lastError = "Failed to open asset file for reading";
      return false;
    }

    AssetHeader header{};
    if (!readPod(stream, header)) {
      m_lastError = "Failed to read asset header";
      return false;
    }

    if (std::memcmp(header.magic, kAssetMagic, sizeof(kAssetMagic)) != 0 || header.version != kAssetVersion) {
      m_lastError = "Unsupported asset format";
      return false;
    }

    if (!hasReasonableCounts(header)) {
      m_lastError = "Asset header contains unreasonable object counts";
      return false;
    }

    SectionTable table{};
    if (!readPod(stream, table)) {
      m_lastError = "Failed to read section table";
      return false;
    }

    const uint64_t assetFileSize = std::filesystem::file_size(assetPath);
    if(!hasValidSectionTable(header, table, assetFileSize)) {
      m_lastError = "Asset section table is corrupt or incomplete";
      return false;
    }

    SceneAsset loaded{};
    loaded.meshes.resize(header.meshCount);
    loaded.materials.resize(header.materialCount);
    loaded.textures.resize(header.textureCount);
    loaded.nodes.resize(header.nodeCount);
    loaded.rootNodes.resize(header.rootNodeCount);
    loaded.dependencies.resize(header.dependencyCount);
    loaded.vertexPayload.resize(static_cast<size_t>(header.vertexPayloadBytes));
    loaded.indexPayload.resize(static_cast<size_t>(header.indexPayloadBytes));
    loaded.texturePayload.resize(static_cast<size_t>(header.texturePayloadBytes));

    // Read meshes
    stream.seekg(static_cast<std::streamoff>(table.meshesOffset), std::ios::beg);
    if (header.meshCount > 0) {
      stream.read(reinterpret_cast<char*>(loaded.meshes.data()), static_cast<std::streamsize>(table.meshesSize));
    }

    // Read materials
    stream.seekg(static_cast<std::streamoff>(table.materialsOffset), std::ios::beg);
    for (auto& mat : loaded.materials) {
      if (!readMaterial(stream, mat)) {
        m_lastError = "Failed to read material payload";
        return false;
      }
    }

    // Read textures
    stream.seekg(static_cast<std::streamoff>(table.texturesOffset), std::ios::beg);
    for (auto& tex : loaded.textures) {
      if (!readTexture(stream, tex)) {
        m_lastError = "Failed to read texture payload";
        return false;
      }
    }

    // Read nodes
    stream.seekg(static_cast<std::streamoff>(table.nodesOffset), std::ios::beg);
    for (auto& node : loaded.nodes) {
      if (!readNode(stream, node)) {
        m_lastError = "Failed to read node payload";
        return false;
      }
    }

    // Read root nodes
    stream.seekg(static_cast<std::streamoff>(table.rootNodesOffset), std::ios::beg);
    if (header.rootNodeCount > 0) {
      stream.read(reinterpret_cast<char*>(loaded.rootNodes.data()), static_cast<std::streamsize>(table.rootNodesSize));
    }

    // Read dependency metadata
    stream.seekg(static_cast<std::streamoff>(table.dependenciesOffset), std::ios::beg);
    for(SceneDependency& dependency : loaded.dependencies) {
      if(!readDependency(stream, dependency)) {
        m_lastError = "Failed to read dependency metadata";
        return false;
      }
    }

    // Read payloads
    stream.seekg(static_cast<std::streamoff>(table.vertexPayloadOffset), std::ios::beg);
    if (header.vertexPayloadBytes > 0) {
      stream.read(reinterpret_cast<char*>(loaded.vertexPayload.data()), static_cast<std::streamsize>(loaded.vertexPayload.size()));
    }

    stream.seekg(static_cast<std::streamoff>(table.indexPayloadOffset), std::ios::beg);
    if (header.indexPayloadBytes > 0) {
      stream.read(reinterpret_cast<char*>(loaded.indexPayload.data()), static_cast<std::streamsize>(loaded.indexPayload.size()));
    }

    stream.seekg(static_cast<std::streamoff>(table.texturePayloadOffset), std::ios::beg);
    if (header.texturePayloadBytes > 0) {
      stream.read(reinterpret_cast<char*>(loaded.texturePayload.data()), static_cast<std::streamsize>(loaded.texturePayload.size()));
    }

    if (!stream) {
      m_lastError = "Failed while reading asset payload";
      return false;
    }

    asset = std::move(loaded);
    return true;
  } catch (const std::bad_alloc&) {
    m_lastError = "Asset allocation exceeded sane limits";
    return false;
  } catch (const std::exception& e) {
    m_lastError = e.what();
    return false;
  }
}

bool SceneAssetSerializer::isValid(const std::filesystem::path& assetPath, const std::filesystem::path& sourcePath) {
  m_lastError.clear();

  try {
    if (!std::filesystem::exists(assetPath) || !std::filesystem::exists(sourcePath)) {
      return false;
    }

    std::ifstream stream(assetPath, std::ios::binary);
    if (!stream) {
      m_lastError = "Failed to open asset file for validation";
      return false;
    }

    AssetHeader header{};
    if (!readPod(stream, header)) {
      m_lastError = "Failed to read asset header for validation";
      return false;
    }

    if (std::memcmp(header.magic, kAssetMagic, sizeof(kAssetMagic)) != 0 || header.version != kAssetVersion) {
      return false;
    }

    if (!hasReasonableCounts(header)) {
      return false;
    }

    SectionTable table{};
    if (!readPod(stream, table)) {
      m_lastError = "Failed to read section table for validation";
      return false;
    }

    const uint64_t assetFileSize = std::filesystem::file_size(assetPath);
    if(!hasValidSectionTable(header, table, assetFileSize)) {
      return false;
    }

    const std::filesystem::path sourceDirectory = sourcePath.parent_path();
    stream.seekg(static_cast<std::streamoff>(table.dependenciesOffset), std::ios::beg);
    for(uint32_t dependencyIndex = 0; dependencyIndex < header.dependencyCount; ++dependencyIndex) {
      SceneDependency dependency;
      if(!readDependency(stream, dependency)) {
        m_lastError = "Failed to read dependency metadata for validation";
        return false;
      }
      if(!dependencyMatchesSource(dependency, sourceDirectory)) {
        m_lastError = "Scene asset dependency changed";
        return false;
      }
    }

    const uint64_t sourceFileSize = std::filesystem::file_size(sourcePath);
    const int64_t  sourceWriteTime = fileWriteTicks(sourcePath);
    const uint64_t sourcePathHash = hashPath(sourcePath);

    return header.sourceFileSize == sourceFileSize
        && header.sourceWriteTimeTicks == sourceWriteTime
        && header.sourcePathHash == sourcePathHash;
  } catch (const std::exception& e) {
    m_lastError = e.what();
    return false;
  }
}

} // namespace demo
