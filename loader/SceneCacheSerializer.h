#pragma once

#include "GltfLoader.h"

#include <filesystem>
#include <string>

namespace demo {

class SceneCacheSerializer
{
public:
  // Bumped 9->10: imported emissive factors now include KHR emissive strength.
  static constexpr uint32_t kCurrentVersion = 10;

  [[nodiscard]] static std::filesystem::path buildCachePath(const std::filesystem::path& sourcePath);

  bool saveCache(const std::filesystem::path& cachePath,
                 const GltfModel&             model,
                 const std::filesystem::path& sourcePath);

  bool loadCache(const std::filesystem::path& cachePath, GltfModel& model);

  bool isCacheValid(const std::filesystem::path& cachePath, const std::filesystem::path& sourcePath);

  [[nodiscard]] const std::string& getLastError() const { return m_lastError; }

private:
  std::string m_lastError;
};

}  // namespace demo
