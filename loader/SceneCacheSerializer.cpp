#include "SceneCacheSerializer.h"

#include <cstring>
#include <fstream>
#include <type_traits>

namespace demo {

namespace {

constexpr char     kCacheMagic[8] = {'V', 'K', 'C', 'A', 'C', 'H', 'E', 0};
constexpr uint32_t kCacheVersion  = SceneCacheSerializer::kCurrentVersion;

struct CacheHeader
{
  char     magic[8]{};
  uint32_t version{0};
  uint32_t reserved{0};
  uint64_t sourceFileSize{0};
  int64_t  sourceWriteTimeTicks{0};
  uint64_t sourcePathHash{0};
  uint32_t meshCount{0};
  uint32_t materialCount{0};
  uint32_t imageCount{0};
  uint32_t nodeCount{0};
  uint32_t rootNodeCount{0};
};

[[nodiscard]] uint64_t hashPath(const std::filesystem::path& path)
{
  return static_cast<uint64_t>(std::hash<std::string>{}(std::filesystem::weakly_canonical(path).generic_string()));
}

[[nodiscard]] int64_t fileWriteTicks(const std::filesystem::path& path)
{
  return static_cast<int64_t>(std::filesystem::last_write_time(path).time_since_epoch().count());
}

template <typename T>
void writePod(std::ostream& stream, const T& value)
{
  static_assert(std::is_trivially_copyable_v<T>, "writePod requires trivially copyable types");
  stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool readPod(std::istream& stream, T& value)
{
  static_assert(std::is_trivially_copyable_v<T>, "readPod requires trivially copyable types");
  stream.read(reinterpret_cast<char*>(&value), sizeof(T));
  return static_cast<bool>(stream);
}

void writeString(std::ostream& stream, const std::string& value)
{
  const uint64_t size = static_cast<uint64_t>(value.size());
  writePod(stream, size);
  if(size > 0)
  {
    stream.write(value.data(), static_cast<std::streamsize>(size));
  }
}

bool readString(std::istream& stream, std::string& value)
{
  uint64_t size = 0;
  if(!readPod(stream, size))
  {
    return false;
  }

  value.resize(static_cast<size_t>(size));
  if(size > 0)
  {
    stream.read(value.data(), static_cast<std::streamsize>(size));
  }
  return static_cast<bool>(stream);
}

template <typename T>
void writeVector(std::ostream& stream, const std::vector<T>& values)
{
  static_assert(std::is_trivially_copyable_v<T>, "writeVector requires trivially copyable element types");
  const uint64_t size = static_cast<uint64_t>(values.size());
  writePod(stream, size);
  if(size > 0)
  {
    stream.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(sizeof(T) * values.size()));
  }
}

template <typename T>
bool readVector(std::istream& stream, std::vector<T>& values)
{
  static_assert(std::is_trivially_copyable_v<T>, "readVector requires trivially copyable element types");
  uint64_t size = 0;
  if(!readPod(stream, size))
  {
    return false;
  }

  values.resize(static_cast<size_t>(size));
  if(size > 0)
  {
    stream.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(sizeof(T) * values.size()));
  }
  return static_cast<bool>(stream);
}

void writeMesh(std::ostream& stream, const GltfMeshData& mesh)
{
  writeVector(stream, mesh.positions);
  writeVector(stream, mesh.normals);
  writeVector(stream, mesh.texCoords);
  writeVector(stream, mesh.tangents);
  writeVector(stream, mesh.indices);
  writePod(stream, mesh.materialIndex);
  writePod(stream, mesh.transform);
}

bool readMesh(std::istream& stream, GltfMeshData& mesh)
{
  return readVector(stream, mesh.positions)
         && readVector(stream, mesh.normals)
         && readVector(stream, mesh.texCoords)
         && readVector(stream, mesh.tangents)
         && readVector(stream, mesh.indices)
         && readPod(stream, mesh.materialIndex)
         && readPod(stream, mesh.transform);
}

void writeMaterial(std::ostream& stream, const GltfMaterialData& material)
{
  writePod(stream, material.baseColorTexture);
  writePod(stream, material.baseColorFactor);
  writePod(stream, material.metallicRoughnessTexture);
  writePod(stream, material.metallicFactor);
  writePod(stream, material.roughnessFactor);
  writePod(stream, material.normalTexture);
  writePod(stream, material.normalScale);
  writePod(stream, material.occlusionTexture);
  writePod(stream, material.occlusionStrength);
  writePod(stream, material.emissiveTexture);
  writePod(stream, material.emissiveFactor);
  writePod(stream, material.doubleSided);
  writeString(stream, material.name);
  writePod(stream, material.alphaMode);
  writePod(stream, material.alphaCutoff);
}

bool readMaterial(std::istream& stream, GltfMaterialData& material)
{
  return readPod(stream, material.baseColorTexture)
         && readPod(stream, material.baseColorFactor)
         && readPod(stream, material.metallicRoughnessTexture)
         && readPod(stream, material.metallicFactor)
         && readPod(stream, material.roughnessFactor)
         && readPod(stream, material.normalTexture)
         && readPod(stream, material.normalScale)
         && readPod(stream, material.occlusionTexture)
         && readPod(stream, material.occlusionStrength)
         && readPod(stream, material.emissiveTexture)
         && readPod(stream, material.emissiveFactor)
         && readPod(stream, material.doubleSided)
         && readString(stream, material.name)
         && readPod(stream, material.alphaMode)
         && readPod(stream, material.alphaCutoff);
}

void writeImage(std::ostream& stream, const GltfImageData& image)
{
  writeVector(stream, image.pixels);
  writePod(stream, image.width);
  writePod(stream, image.height);
  writePod(stream, image.channels);
  writeString(stream, image.uri);
}

bool readImage(std::istream& stream, GltfImageData& image)
{
  return readVector(stream, image.pixels)
         && readPod(stream, image.width)
         && readPod(stream, image.height)
         && readPod(stream, image.channels)
         && readString(stream, image.uri);
}

void writeNode(std::ostream& stream, const GltfNodeData& node)
{
  writeString(stream, node.name);
  writePod(stream, node.parent);
  writeVector(stream, node.children);
  writePod(stream, node.translation);
  writePod(stream, node.rotation);
  writePod(stream, node.rotationEulerDegrees);
  writePod(stream, node.scale);
  writePod(stream, node.localTransform);
  writePod(stream, node.worldTransform);
  writePod(stream, node.firstMeshIndex);
  writePod(stream, node.meshCount);
}

bool readNode(std::istream& stream, GltfNodeData& node)
{
  return readString(stream, node.name)
         && readPod(stream, node.parent)
         && readVector(stream, node.children)
         && readPod(stream, node.translation)
         && readPod(stream, node.rotation)
         && readPod(stream, node.rotationEulerDegrees)
         && readPod(stream, node.scale)
         && readPod(stream, node.localTransform)
         && readPod(stream, node.worldTransform)
         && readPod(stream, node.firstMeshIndex)
         && readPod(stream, node.meshCount);
}

}  // namespace

std::filesystem::path SceneCacheSerializer::buildCachePath(const std::filesystem::path& sourcePath)
{
  return sourcePath.parent_path() / (sourcePath.filename().string() + ".vkcache");
}

bool SceneCacheSerializer::saveCache(const std::filesystem::path& cachePath,
                                     const GltfModel&             model,
                                     const std::filesystem::path& sourcePath)
{
  m_lastError.clear();

  try
  {
    std::filesystem::create_directories(cachePath.parent_path());
  }
  catch(const std::exception& e)
  {
    m_lastError = e.what();
    return false;
  }

  std::ofstream stream(cachePath, std::ios::binary | std::ios::trunc);
  if(!stream)
  {
    m_lastError = "Failed to open cache file for writing";
    return false;
  }

  CacheHeader header{};
  std::memcpy(header.magic, kCacheMagic, sizeof(kCacheMagic));
  header.version              = kCacheVersion;
  header.sourceFileSize       = std::filesystem::exists(sourcePath) ? std::filesystem::file_size(sourcePath) : 0;
  header.sourceWriteTimeTicks = std::filesystem::exists(sourcePath) ? fileWriteTicks(sourcePath) : 0;
  header.sourcePathHash       = hashPath(sourcePath);
  header.meshCount            = static_cast<uint32_t>(model.meshes.size());
  header.materialCount        = static_cast<uint32_t>(model.materials.size());
  header.imageCount           = static_cast<uint32_t>(model.images.size());
  header.nodeCount            = static_cast<uint32_t>(model.nodes.size());
  header.rootNodeCount        = static_cast<uint32_t>(model.rootNodes.size());
  writePod(stream, header);

  writeString(stream, model.name);
  writeString(stream, model.sourcePath);
  writeString(stream, model.sourceDirectory);

  for(const auto& mesh : model.meshes)
  {
    writeMesh(stream, mesh);
  }
  for(const auto& material : model.materials)
  {
    writeMaterial(stream, material);
  }
  for(const auto& image : model.images)
  {
    writeImage(stream, image);
  }
  for(const auto& node : model.nodes)
  {
    writeNode(stream, node);
  }
  writeVector(stream, model.rootNodes);

  if(!stream)
  {
    m_lastError = "Failed while writing cache payload";
    return false;
  }

  return true;
}

bool SceneCacheSerializer::loadCache(const std::filesystem::path& cachePath, GltfModel& model)
{
  m_lastError.clear();

  std::ifstream stream(cachePath, std::ios::binary);
  if(!stream)
  {
    m_lastError = "Failed to open cache file for reading";
    return false;
  }

  CacheHeader header{};
  if(!readPod(stream, header))
  {
    m_lastError = "Failed to read cache header";
    return false;
  }

  if(std::memcmp(header.magic, kCacheMagic, sizeof(kCacheMagic)) != 0 || header.version != kCacheVersion)
  {
    m_lastError = "Unsupported cache format";
    return false;
  }

  GltfModel loadedModel{};
  loadedModel.meshes.resize(header.meshCount);
  loadedModel.materials.resize(header.materialCount);
  loadedModel.images.resize(header.imageCount);
  loadedModel.nodes.resize(header.nodeCount);

  if(!readString(stream, loadedModel.name))
  {
    m_lastError = "Failed to read model name";
    return false;
  }
  if(!readString(stream, loadedModel.sourcePath) || !readString(stream, loadedModel.sourceDirectory))
  {
    m_lastError = "Failed to read model source metadata";
    return false;
  }

  for(auto& mesh : loadedModel.meshes)
  {
    if(!readMesh(stream, mesh))
    {
      m_lastError = "Failed to read mesh payload";
      return false;
    }
  }
  for(auto& material : loadedModel.materials)
  {
    if(!readMaterial(stream, material))
    {
      m_lastError = "Failed to read material payload";
      return false;
    }
  }
  for(auto& image : loadedModel.images)
  {
    if(!readImage(stream, image))
    {
      m_lastError = "Failed to read image payload";
      return false;
    }
  }
  for(auto& node : loadedModel.nodes)
  {
    if(!readNode(stream, node))
    {
      m_lastError = "Failed to read node payload";
      return false;
    }
  }
  if(!readVector(stream, loadedModel.rootNodes))
  {
    m_lastError = "Failed to read root node payload";
    return false;
  }

  model = std::move(loadedModel);
  return true;
}

bool SceneCacheSerializer::isCacheValid(const std::filesystem::path& cachePath, const std::filesystem::path& sourcePath)
{
  m_lastError.clear();

  if(!std::filesystem::exists(cachePath) || !std::filesystem::exists(sourcePath))
  {
    return false;
  }

  std::ifstream stream(cachePath, std::ios::binary);
  if(!stream)
  {
    m_lastError = "Failed to open cache file for validation";
    return false;
  }

  CacheHeader header{};
  if(!readPod(stream, header))
  {
    m_lastError = "Failed to read cache header for validation";
    return false;
  }

  if(std::memcmp(header.magic, kCacheMagic, sizeof(kCacheMagic)) != 0 || header.version != kCacheVersion)
  {
    return false;
  }

  const uint64_t sourceFileSize = std::filesystem::file_size(sourcePath);
  const int64_t  sourceWriteTime = fileWriteTicks(sourcePath);
  const uint64_t sourcePathHash = hashPath(sourcePath);

  return header.sourceFileSize == sourceFileSize
         && header.sourceWriteTimeTicks == sourceWriteTime
         && header.sourcePathHash == sourcePathHash;
}

}  // namespace demo
