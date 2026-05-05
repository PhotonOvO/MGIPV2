#pragma once

#include "../common/Common.h"

#include <filesystem>
#include <string>

namespace demo {

class Ktx2Loader
{
public:
  struct Ktx2Texture
  {
    VkFormat                format{VK_FORMAT_UNDEFINED};
    uint32_t                width{0};
    uint32_t                height{0};
    uint32_t                mipLevels{0};
    std::vector<VkDeviceSize> mipOffsets;
    std::vector<VkDeviceSize> mipSizes;
    std::vector<uint8_t>    data;
  };

  [[nodiscard]] static std::filesystem::path buildSidecarPath(const std::filesystem::path& sourceDirectory,
                                                              const std::string&          imageUri);

  bool load(const std::filesystem::path& filepath, Ktx2Texture& outTexture);
  [[nodiscard]] const std::string& getLastError() const { return m_lastError; }

private:
  std::string m_lastError;
};

}  // namespace demo
