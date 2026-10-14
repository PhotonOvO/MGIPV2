#pragma once

#include "SceneAsset.h"

#include <filesystem>
#include <string>

namespace demo {

class SceneAssetSerializer {
public:
  // Bumped for projection identity, primitive, visibility, variant,
  // animation, source semantic-hash, texture source-image, and camera/light
  // source-identity fields.
  static constexpr uint32_t kCurrentVersion = 12;

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
