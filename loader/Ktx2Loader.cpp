#include "Ktx2Loader.h"

#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <type_traits>

namespace demo {

namespace {

constexpr std::array<uint8_t, 12> kKtx2Identifier{
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

struct Ktx2Header
{
  uint8_t  identifier[12]{};
  uint32_t vkFormat{0};
  uint32_t typeSize{0};
  uint32_t pixelWidth{0};
  uint32_t pixelHeight{0};
  uint32_t pixelDepth{0};
  uint32_t layerCount{0};
  uint32_t faceCount{0};
  uint32_t levelCount{0};
  uint32_t supercompressionScheme{0};
  uint32_t dfdByteOffset{0};
  uint32_t dfdByteLength{0};
  uint32_t kvdByteOffset{0};
  uint32_t kvdByteLength{0};
  uint64_t sgdByteOffset{0};
  uint64_t sgdByteLength{0};
};

struct Ktx2LevelIndex
{
  uint64_t byteOffset{0};
  uint64_t byteLength{0};
  uint64_t uncompressedByteLength{0};
};

template <typename T>
bool readPod(std::istream& stream, T& value)
{
  static_assert(std::is_trivially_copyable_v<T>, "readPod requires trivially copyable types");
  stream.read(reinterpret_cast<char*>(&value), sizeof(T));
  return static_cast<bool>(stream);
}

}  // namespace

std::filesystem::path Ktx2Loader::buildSidecarPath(const std::filesystem::path& sourceDirectory,
                                                   const std::string&          imageUri)
{
  if(imageUri.empty())
  {
    return {};
  }

  std::filesystem::path uriPath(imageUri);
  uriPath.replace_extension(".ktx2");
  return sourceDirectory / uriPath;
}

bool Ktx2Loader::load(const std::filesystem::path& filepath, Ktx2Texture& outTexture)
{
  m_lastError.clear();
  outTexture = {};

  std::ifstream stream(filepath, std::ios::binary);
  if(!stream)
  {
    m_lastError = "Failed to open KTX2 file";
    return false;
  }

  stream.seekg(0, std::ios::end);
  const std::streamoff fileSize = stream.tellg();
  if(fileSize <= 0 || static_cast<uint64_t>(fileSize) > std::numeric_limits<size_t>::max())
  {
    m_lastError = "Invalid KTX2 file size";
    return false;
  }

  std::vector<uint8_t> fileData(static_cast<size_t>(fileSize));
  stream.seekg(0, std::ios::beg);
  stream.read(reinterpret_cast<char*>(fileData.data()), fileSize);
  if(!stream)
  {
    m_lastError = "Failed to read KTX2 file";
    return false;
  }

  return loadFromMemory(fileData.data(), fileData.size(), outTexture);
}

bool Ktx2Loader::loadFromMemory(const uint8_t* data, size_t size, Ktx2Texture& outTexture)
{
  m_lastError.clear();
  outTexture = {};

  if(data == nullptr || size < sizeof(Ktx2Header))
  {
    m_lastError = "Failed to read KTX2 header";
    return false;
  }

  Ktx2Header header{};
  std::memcpy(&header, data, sizeof(Ktx2Header));

  if(std::memcmp(header.identifier, kKtx2Identifier.data(), kKtx2Identifier.size()) != 0)
  {
    m_lastError = "Invalid KTX2 identifier";
    return false;
  }

  if(header.pixelDepth != 0 || header.layerCount > 1 || header.faceCount > 1)
  {
    m_lastError = "Only 2D non-array KTX2 textures are supported";
    return false;
  }

  if(header.supercompressionScheme != 0)
  {
    m_lastError = "Supercompressed KTX2 textures are not supported";
    return false;
  }

  const uint32_t levelCount = std::max(header.levelCount, 1u);
  if(sizeof(Ktx2Header) + static_cast<size_t>(levelCount) * sizeof(Ktx2LevelIndex) > size)
  {
    m_lastError = "Failed to read KTX2 level index";
    return false;
  }

  std::vector<Ktx2LevelIndex> levels(levelCount);
  std::memcpy(levels.data(), data + sizeof(Ktx2Header), levels.size() * sizeof(Ktx2LevelIndex));

  uint64_t totalBytes = 0;
  for(const Ktx2LevelIndex& level : levels)
  {
    if(level.byteOffset > size || level.byteLength > size - level.byteOffset)
    {
      m_lastError = "KTX2 level range exceeds file size";
      return false;
    }
    totalBytes += level.byteLength;
  }

  if(totalBytes > std::numeric_limits<size_t>::max())
  {
    m_lastError = "KTX2 payload is too large";
    return false;
  }

  outTexture.format    = static_cast<VkFormat>(header.vkFormat);
  outTexture.width     = header.pixelWidth;
  outTexture.height    = header.pixelHeight;
  outTexture.mipLevels = levelCount;
  outTexture.mipOffsets.resize(levelCount);
  outTexture.mipSizes.resize(levelCount);
  outTexture.data.resize(static_cast<size_t>(totalBytes));

  uint64_t packedOffset = 0;
  for(uint32_t mip = 0; mip < levelCount; ++mip)
  {
    const Ktx2LevelIndex& level = levels[mip];
    outTexture.mipOffsets[mip]  = packedOffset;
    outTexture.mipSizes[mip]    = level.byteLength;
    std::memcpy(outTexture.data.data() + packedOffset, data + level.byteOffset, static_cast<size_t>(level.byteLength));

    packedOffset += level.byteLength;
  }

  return true;
}

}  // namespace demo
