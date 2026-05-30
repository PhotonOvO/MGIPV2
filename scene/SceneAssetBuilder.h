#pragma once

#include "SceneAsset.h"
#include "../loader/GltfLoader.h"

namespace demo {

class SceneAssetBuilder {
public:
  struct BuildOptions {
    bool buildMeshlets = false;      // Pre-build meshlet data into SceneAsset
    bool keepRawVertexData = false;  // Keep raw separated attributes for meshlet conversion fallback
  };

  [[nodiscard]] static SceneAsset build(const GltfModel& model, const BuildOptions& options = {});

private:
  static void buildMeshes(SceneAsset& asset, const GltfModel& model, const BuildOptions& options);
  static void buildMaterials(SceneAsset& asset, const GltfModel& model);
  static void buildTextures(SceneAsset& asset, const GltfModel& model);
  static void buildNodes(SceneAsset& asset, const GltfModel& model);
  static void buildMeshlets(SceneAsset& asset, const GltfModel& model);
};

}  // namespace demo
