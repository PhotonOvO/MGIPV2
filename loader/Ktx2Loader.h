#pragma once

#include "../common/Common.h"
#include "../rhi/RHITypes.h"   // rhi::TextureFormat — portable, no Vulkan dependency

#include <filesystem>
#include <string>
#include <vector>

namespace demo {

class Ktx2Loader
{
public:
  struct Ktx2Texture
  {
    rhi::TextureFormat      format{rhi::TextureFormat::undefined};
    uint32_t                width{0};
    uint32_t                height{0};
    uint32_t                mipLevels{0};
    std::vector<uint64_t>   mipOffsets;   // uint64_t (was platform size type, zero semantic change)
    std::vector<uint64_t>   mipSizes;     // same
    std::vector<uint8_t>    data;
  };

  [[nodiscard]] static std::filesystem::path buildSidecarPath(const std::filesystem::path& sourceDirectory,
                                                              const std::string&          imageUri);

  bool load(const std::filesystem::path& filepath, Ktx2Texture& outTexture);
  bool loadFromMemory(const uint8_t* data, size_t size, Ktx2Texture& outTexture);
  [[nodiscard]] const std::string& getLastError() const { return m_lastError; }

private:
  std::string m_lastError;
};

}  // namespace demo
