#pragma once

#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace tinygltf {
class Model;
struct Material;
} // namespace tinygltf

namespace demo::editor::detail {
struct CanonicalGltfMaterialMetadata final {
  std::string prefix;
  std::vector<std::string> materials;
  std::string suffix;
  bool valid{false};

  [[nodiscard]] std::string text() const {
    std::string result = prefix;
    for (std::size_t i = 0u; i < materials.size(); ++i) {
      if (i != 0u)
        result.push_back(',');
      result.append(materials[i]);
    }
    return result.append(suffix);
  }
};

// Serialize metadata using pinned TinyGLTF's normal serializers. Payload URIs
// are supplied by the caller so unchanged large payloads can be reused.
bool writeCanonicalGltfMetadata(const tinygltf::Model &model, std::ostream &stream,
                                std::span<const std::string> bufferUris = {},
                                std::span<const std::string> imageUris = {},
                                CanonicalGltfMaterialMetadata *materialMetadata = nullptr);
[[nodiscard]] std::string canonicalGltfMaterialMetadata(const tinygltf::Material &material);
} // namespace demo::editor::detail
