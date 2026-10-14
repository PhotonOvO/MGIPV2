#pragma once

#include "SceneAsset.h"
#include "../loader/GltfLoader.h"

#include <cstdint>
#include <optional>

namespace demo {

class SceneAssetBuilder {
public:
  struct BuildOptions {
    bool buildMeshlets = false;      // Pre-build meshlet data into SceneAsset
    bool keepRawVertexData = false;  // Keep raw separated attributes for meshlet conversion fallback
    // Runtime-only variant selection. The source GltfModel remains unchanged;
    // the selected material is applied to derived primitives.
    std::optional<std::uint32_t> activeVariant;
  };

  [[nodiscard]] static SceneAsset build(const GltfModel& model);
  [[nodiscard]] static SceneAsset build(const GltfModel& model, const BuildOptions& options);

private:
  static void buildMeshes(SceneAsset& asset, const GltfModel& model, const BuildOptions& options);
  static void buildMaterials(SceneAsset& asset, const GltfModel& model);
  static void buildTextures(SceneAsset& asset, const GltfModel& model);
  static void buildNodes(SceneAsset& asset, const GltfModel& model);
  static void buildMeshlets(SceneAsset& asset, const GltfModel& model);
};

}  // namespace demo
