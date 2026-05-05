#include "Ktx2Loader.h"

#include <array>
#include <cstring>
#include <fstream>
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

  Ktx2Header header{};
  if(!readPod(stream, header))
  {
    m_lastError = "Failed to read KTX2 header";
    return false;
  }

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
  std::vector<Ktx2LevelIndex> levels(levelCount);
  for(Ktx2LevelIndex& level : levels)
  {
    if(!readPod(stream, level))
    {
      m_lastError = "Failed to read KTX2 level index";
      return false;
    }
  }

  stream.seekg(0, std::ios::end);
  const uint64_t fileSize = static_cast<uint64_t>(stream.tellg());

  uint64_t totalBytes = 0;
  for(const Ktx2LevelIndex& level : levels)
  {
    if(level.byteOffset + level.byteLength > fileSize)
    {
      m_lastError = "KTX2 level range exceeds file size";
      return false;
    }
    totalBytes += level.byteLength;
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

    stream.seekg(static_cast<std::streamoff>(level.byteOffset), std::ios::beg);
    stream.read(reinterpret_cast<char*>(outTexture.data.data() + packedOffset),
                static_cast<std::streamsize>(level.byteLength));
    if(!stream)
    {
      m_lastError = "Failed to read KTX2 mip payload";
      return false;
    }

    packedOffset += level.byteLength;
  }

  return true;
}

}  // namespace demo
