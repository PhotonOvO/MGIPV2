#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <nvtt/nvtt.h>

namespace {

constexpr uint32_t kVkFormatBc7UnormBlock = 145u;
constexpr uint32_t kVkFormatBc7SrgbBlock = 146u;

struct Ktx2Header
{
  uint8_t  identifier[12]{};
  uint32_t vkFormat{0};
  uint32_t typeSize{1};
  uint32_t pixelWidth{0};
  uint32_t pixelHeight{0};
  uint32_t pixelDepth{0};
  uint32_t layerCount{0};
  uint32_t faceCount{1};
  uint32_t levelCount{1};
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

struct EncodedMip
{
  int width{0};
  int height{0};
  std::vector<uint8_t> bytes;
};

struct CompressionProfile
{
  nvtt::Quality quality{nvtt::Quality_Normal};
  bool preferCuda{true};
  const char* label{"normal"};
};

class NvttMipCollector final : public nvtt::OutputHandler
{
public:
  void beginImage(int size, int width, int height, int depth, int face, int miplevel) override
  {
    (void)depth;
    (void)face;
    (void)miplevel;
    m_mips.push_back(EncodedMip{});
    m_mips.back().width = width;
    m_mips.back().height = height;
    m_mips.back().bytes.reserve(static_cast<size_t>(std::max(size, 0)));
  }

  bool writeData(const void* data, int size) override
  {
    if(m_mips.empty() || data == nullptr || size < 0)
    {
      return false;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    m_mips.back().bytes.insert(m_mips.back().bytes.end(), bytes, bytes + size);
    return true;
  }

  void endImage() override {}

  [[nodiscard]] const std::vector<EncodedMip>& mipChain() const
  {
    return m_mips;
  }

private:
  std::vector<EncodedMip> m_mips;
};

bool isSupportedTexture(const std::filesystem::path& path)
{
  const std::string ext = path.extension().string();
  return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".PNG" || ext == ".JPG" || ext == ".JPEG";
}

std::string toLower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool stemContains(const std::string& stem, const char* token)
{
  return stem.find(token) != std::string::npos;
}

bool isSrgbTexture(const std::filesystem::path& inputPath)
{
  const std::string stem = toLower(inputPath.stem().string());
  return stemContains(stem, "albedo") || stemContains(stem, "basecolor") || stemContains(stem, "base_color")
         || stemContains(stem, "diffuse") || stemContains(stem, "emissive");
}

bool isNormalTexture(const std::filesystem::path& inputPath)
{
  const std::string stem = toLower(inputPath.stem().string());
  return stemContains(stem, "normal") || stemContains(stem, "nrm");
}

uint32_t selectVkFormat(const std::filesystem::path& inputPath)
{
  return isSrgbTexture(inputPath) ? kVkFormatBc7SrgbBlock : kVkFormatBc7UnormBlock;
}

bool loadImageRgba8(const std::filesystem::path& inputPath, int& outWidth, int& outHeight, std::vector<uint8_t>& outPixels)
{
  outWidth = 0;
  outHeight = 0;
  outPixels.clear();

  int channels = 0;
  stbi_uc* pixels = stbi_load(inputPath.string().c_str(), &outWidth, &outHeight, &channels, 4);
  if(pixels == nullptr)
  {
    return false;
  }

  const size_t pixelCount = static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight) * 4u;
  outPixels.assign(pixels, pixels + pixelCount);
  stbi_image_free(pixels);
  return true;
}

std::vector<uint8_t> rgbaToBgra(const std::vector<uint8_t>& rgbaPixels)
{
  std::vector<uint8_t> bgraPixels = rgbaPixels;
  for(size_t i = 0; i + 3 < bgraPixels.size(); i += 4)
  {
    std::swap(bgraPixels[i + 0], bgraPixels[i + 2]);
  }
  return bgraPixels;
}

std::vector<nvtt::Surface> buildMipChain(const std::filesystem::path& inputPath,
                                         int                         width,
                                         int                         height,
                                         const std::vector<uint8_t>& bgraPixels)
{
  nvtt::Surface baseSurface;
  if(!baseSurface.setImage(nvtt::InputFormat_BGRA_8UB, width, height, 1, bgraPixels.data()))
  {
    throw std::runtime_error("NVTT failed to initialize source surface");
  }

  baseSurface.setAlphaMode(nvtt::AlphaMode_Transparency);
  baseSurface.setNormalMap(isNormalTexture(inputPath));

  std::vector<nvtt::Surface> mipChain;
  mipChain.push_back(baseSurface);
  while(mipChain.back().canMakeNextMipmap())
  {
    nvtt::Surface nextMip = mipChain.back().clone();
    if(!nextMip.buildNextMipmap(nvtt::MipmapFilter_Box))
    {
      break;
    }
    mipChain.push_back(nextMip);
  }

  return mipChain;
}

std::vector<EncodedMip> compressMipChain(const std::filesystem::path& inputPath,
                                         const std::vector<nvtt::Surface>& mipChain,
                                         bool useCuda,
                                         nvtt::Quality quality)
{
  nvtt::CompressionOptions compressionOptions;
  compressionOptions.setFormat(nvtt::Format_BC7);
  compressionOptions.setQuality(quality);

  nvtt::OutputOptions outputOptions;
  outputOptions.setOutputHeader(false);
  outputOptions.setContainer(nvtt::Container_DDS10);
  outputOptions.setSrgbFlag(isSrgbTexture(inputPath));

  NvttMipCollector collector;
  outputOptions.setOutputHandler(&collector);

  nvtt::Context context(useCuda);
  context.enableCudaAcceleration(useCuda);
  for(size_t mipIndex = 0; mipIndex < mipChain.size(); ++mipIndex)
  {
    if(!context.compress(mipChain[mipIndex], 0, static_cast<int>(mipIndex), compressionOptions, outputOptions))
    {
      throw std::runtime_error("NVTT BC7 compression failed");
    }
  }

  return collector.mipChain();
}

void writeKtx2File(const std::filesystem::path& outputPath,
                   uint32_t                     vkFormat,
                   int                          width,
                   int                          height,
                   const std::vector<EncodedMip>& mipChain)
{
  static constexpr std::array<uint8_t, 12> kIdentifier{
      0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

  if(mipChain.empty())
  {
    throw std::runtime_error("Refusing to write empty mip chain");
  }

  Ktx2Header header{};
  std::memcpy(header.identifier, kIdentifier.data(), kIdentifier.size());
  header.vkFormat = vkFormat;
  header.typeSize = 1u;
  header.pixelWidth = static_cast<uint32_t>(width);
  header.pixelHeight = static_cast<uint32_t>(height);
  header.pixelDepth = 0u;
  header.layerCount = 0u;
  header.faceCount = 1u;
  header.levelCount = static_cast<uint32_t>(mipChain.size());

  std::vector<Ktx2LevelIndex> levels(mipChain.size());
  uint64_t payloadOffset = sizeof(Ktx2Header) + static_cast<uint64_t>(levels.size()) * sizeof(Ktx2LevelIndex);
  for(size_t mipIndex = 0; mipIndex < mipChain.size(); ++mipIndex)
  {
    levels[mipIndex].byteOffset = payloadOffset;
    levels[mipIndex].byteLength = mipChain[mipIndex].bytes.size();
    levels[mipIndex].uncompressedByteLength = mipChain[mipIndex].bytes.size();
    payloadOffset += levels[mipIndex].byteLength;
  }

  std::ofstream stream(outputPath, std::ios::binary);
  if(!stream)
  {
    throw std::runtime_error("Failed to open output file for writing");
  }

  stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
  stream.write(reinterpret_cast<const char*>(levels.data()),
               static_cast<std::streamsize>(levels.size() * sizeof(Ktx2LevelIndex)));
  for(const EncodedMip& mip : mipChain)
  {
    stream.write(reinterpret_cast<const char*>(mip.bytes.data()), static_cast<std::streamsize>(mip.bytes.size()));
  }

  if(!stream)
  {
    throw std::runtime_error("Failed while writing KTX2 payload");
  }
}

}  // namespace

int main(int argc, char** argv)
{
  if(argc < 3)
  {
    std::cerr << "Usage: texture_converter <input_dir> <output_dir> [--quality fastest|normal|production|highest]\n";
    std::cerr << "Offline-compresses PNG/JPG textures to BC7 KTX2 sidecars with NVTT-generated mip chains.\n";
    return 1;
  }

  const std::filesystem::path inputDir = argv[1];
  const std::filesystem::path outputDir = argv[2];
  CompressionProfile profile{};
  for(int argIndex = 3; argIndex < argc; ++argIndex)
  {
    const std::string arg = toLower(argv[argIndex]);
    if(arg == "--quality")
    {
      if(argIndex + 1 >= argc)
      {
        std::cerr << "--quality requires a value.\n";
        return 1;
      }

      const std::string qualityArg = toLower(argv[++argIndex]);
      if(qualityArg == "fastest")
      {
        profile.quality = nvtt::Quality_Fastest;
        profile.preferCuda = true;
        profile.label = "fastest";
      }
      else if(qualityArg == "normal")
      {
        profile.quality = nvtt::Quality_Normal;
        profile.preferCuda = true;
        profile.label = "normal";
      }
      else if(qualityArg == "production")
      {
        profile.quality = nvtt::Quality_Production;
        profile.preferCuda = false;
        profile.label = "production";
      }
      else if(qualityArg == "highest")
      {
        profile.quality = nvtt::Quality_Highest;
        profile.preferCuda = false;
        profile.label = "highest";
      }
      else
      {
        std::cerr << "Unsupported quality: " << qualityArg << "\n";
        return 1;
      }
    }
    else
    {
      std::cerr << "Unknown argument: " << arg << "\n";
      return 1;
    }
  }

  const bool cudaSupported = nvtt::isCudaSupported();
  const bool useCuda = cudaSupported && profile.preferCuda;

  if(!std::filesystem::exists(inputDir))
  {
    std::cerr << "Input directory does not exist: " << inputDir << "\n";
    return 1;
  }

  std::filesystem::create_directories(outputDir);
  std::cout << "NVTT backend: " << (useCuda ? "CUDA" : "CPU")
            << " quality=" << profile.label << "\n";

  uint32_t convertedCount = 0;
  for(const auto& entry : std::filesystem::recursive_directory_iterator(inputDir))
  {
    if(!entry.is_regular_file() || !isSupportedTexture(entry.path()))
    {
      continue;
    }

    const std::filesystem::path relativePath = std::filesystem::relative(entry.path(), inputDir);
    std::filesystem::path outputPath = outputDir / relativePath;
    outputPath.replace_extension(".ktx2");
    std::filesystem::create_directories(outputPath.parent_path());

    try
    {
      int width = 0;
      int height = 0;
      std::vector<uint8_t> rgbaPixels;
      if(!loadImageRgba8(entry.path(), width, height, rgbaPixels))
      {
        std::cerr << "Failed to decode image: " << entry.path() << " (" << stbi_failure_reason() << ")\n";
        return 1;
      }

      const std::vector<uint8_t> bgraPixels = rgbaToBgra(rgbaPixels);
      const std::vector<nvtt::Surface> mipChain = buildMipChain(entry.path(), width, height, bgraPixels);
      const std::vector<EncodedMip> encodedMips = compressMipChain(entry.path(), mipChain, useCuda, profile.quality);
      const uint32_t vkFormat = selectVkFormat(entry.path());

      std::cout << "Converting " << entry.path() << " -> " << outputPath << " ("
                << (vkFormat == kVkFormatBc7SrgbBlock ? "BC7_SRGB" : "BC7_UNORM")
                << ", mips=" << encodedMips.size() << ")\n";
      writeKtx2File(outputPath, vkFormat, width, height, encodedMips);
      ++convertedCount;
    }
    catch(const std::exception& e)
    {
      std::cerr << "Conversion failed for " << entry.path() << ": " << e.what() << "\n";
      return 1;
    }
  }

  std::cout << "Converted " << convertedCount << " textures.\n";
  return 0;
}
