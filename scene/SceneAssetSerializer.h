#pragma once

#include "SceneAsset.h"

#include <filesystem>
#include <string>

namespace demo {

class SceneAssetSerializer {
public:
  static constexpr uint32_t kCurrentVersion = 3;

  [[nodiscard]] static std::filesystem::path buildAssetPath(const std::filesystem::path& sourcePath);

  bool save(const std::filesystem::path& assetPath,
            const SceneAsset&            asset,
            const std::filesystem::path& sourcePath);

  bool load(const std::filesystem::path& assetPath, SceneAsset& asset);

  bool isValid(const std::filesystem::path& assetPath, const std::filesystem::path& sourcePath);

  [[nodiscard]] const std::string& getLastError() const { return m_lastError; }

private:
  std::string m_lastError;
};

}  // namespace demo
