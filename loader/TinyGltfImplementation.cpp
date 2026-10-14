#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tiny_gltf.h>

#include "GltfCanonicalWriter.h"

bool demo::editor::detail::writeCanonicalGltfMetadata(
    const tinygltf::Model &model, std::ostream &stream, std::span<const std::string> bufferUris,
    std::span<const std::string> imageUris, CanonicalGltfMaterialMetadata *materialMetadata) {
  if ((!bufferUris.empty() && bufferUris.size() != model.buffers.size()) ||
      (!imageUris.empty() && imageUris.size() != model.images.size()))
    return false;
  tinygltf::detail::JsonDocument output;
  tinygltf::SerializeGltfModel(&model, output);
  if (!model.buffers.empty()) {
    tinygltf::detail::json buffers;
    tinygltf::detail::JsonReserveArray(buffers, model.buffers.size());
    for (std::size_t index = 0; index < model.buffers.size(); ++index) {
      const auto &source = model.buffers[index];
      tinygltf::detail::json buffer;
      tinygltf::SerializeNumberProperty("byteLength", source.data.size(), buffer);
      tinygltf::SerializeStringProperty("uri", bufferUris.empty() ? source.uri : bufferUris[index],
                                        buffer);
      if (!source.name.empty())
        tinygltf::SerializeStringProperty("name", source.name, buffer);
      tinygltf::SerializeExtrasAndExtensions(source, buffer);
      tinygltf::detail::JsonPushBack(buffers, std::move(buffer));
    }
    tinygltf::detail::JsonAddMember(output, "buffers", std::move(buffers));
  }
  if (!model.images.empty()) {
    tinygltf::detail::json images;
    tinygltf::detail::JsonReserveArray(images, model.images.size());
    for (std::size_t index = 0; index < model.images.size(); ++index) {
      const auto &source = model.images[index];
      tinygltf::detail::json image;
      tinygltf::SerializeGltfImage(source, imageUris.empty() ? source.uri : imageUris[index],
                                   image);
      tinygltf::detail::JsonPushBack(images, std::move(image));
    }
    tinygltf::detail::JsonAddMember(output, "images", std::move(images));
  }
  const auto serialized = tinygltf::detail::JsonToString(output, -1);
  if (materialMetadata != nullptr) {
    CanonicalGltfMaterialMetadata cached;
    cached.prefix = "{";
    std::string *destination = &cached.prefix;
    bool first = true;
    const auto &root = output;
    for (auto it = tinygltf::detail::ObjectBegin(root); it != tinygltf::detail::ObjectEnd(root);
         ++it) {
      if (!first)
        destination->push_back(',');
      first = false;
      const auto key = tinygltf::detail::GetKey(it);
      destination->append(
          tinygltf::detail::JsonToString(tinygltf::detail::JsonFromString(key.c_str())));
      destination->push_back(':');
      const auto &value = tinygltf::detail::GetValue(it);
      if (key == "materials") {
        destination->push_back('[');
        for (auto material = tinygltf::detail::ArrayBegin(value);
             material != tinygltf::detail::ArrayEnd(value); ++material)
          cached.materials.push_back(tinygltf::detail::JsonToString(*material, -1));
        destination = &cached.suffix;
        destination->push_back(']');
      } else {
        destination->append(tinygltf::detail::JsonToString(value, -1));
      }
    }
    destination->push_back('}');
    cached.valid = !cached.materials.empty() && cached.text() == serialized;
    // WriteGltfStream adds a newline after the compact JSON object.
    cached.suffix.push_back('\n');
    *materialMetadata = std::move(cached);
  }
  return tinygltf::WriteGltfStream(stream, serialized);
}

std::string
demo::editor::detail::canonicalGltfMaterialMetadata(const tinygltf::Material &material) {
  tinygltf::detail::json output;
  tinygltf::SerializeGltfMaterial(material, output);
  return tinygltf::detail::JsonToString(output, -1);
}
