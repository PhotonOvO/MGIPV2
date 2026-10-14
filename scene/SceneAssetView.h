#pragma once

#include "SceneAsset.h"

#include <span>
#include <string>
#include <string_view>

namespace demo {

struct SceneAssetView {
  std::span<const SceneMesh> meshes;
  std::span<const ScenePrimitive> primitives;
  std::span<const SceneMaterial> materials;
  std::span<const SceneTexture> textures;
  std::span<const SceneNode> nodes;
  std::span<const SceneCamera> cameras;
  std::span<const SceneLight> lights;
  std::span<const uint32_t> rootNodes;
  std::span<const uint8_t> sceneRootTransformApplyToRoot;
  std::span<const std::string> materialVariantNames;
  std::span<const SceneAnimation> animations;

  std::string_view documentSemanticHash;
  uint64_t documentRevision = 0;
  SceneRootTransformMetadata sceneRootTransform;

  std::span<const uint8_t> vertexPayload;
  std::span<const uint8_t> indexPayload;
  std::span<const uint8_t> texturePayload;
};

struct SceneAssetValidationResult {
  bool        valid{true};
  std::string error;
};

[[nodiscard]] inline SceneAssetView makeSceneAssetView(const SceneAsset& asset)
{
  return SceneAssetView{
      .meshes = asset.meshes,
      .primitives = asset.primitives,
      .materials = asset.materials,
      .textures = asset.textures,
      .nodes = asset.nodes,
      .cameras = asset.cameras,
      .lights = asset.lights,
      .rootNodes = asset.rootNodes,
      .sceneRootTransformApplyToRoot = asset.sceneRootTransformApplyToRoot,
      .materialVariantNames = asset.materialVariantNames,
      .animations = asset.animations,
      .documentSemanticHash = asset.documentSemanticHash,
      .documentRevision = asset.documentRevision,
      .sceneRootTransform = asset.sceneRootTransform,
      .vertexPayload = asset.vertexPayload,
      .indexPayload = asset.indexPayload,
      .texturePayload = asset.texturePayload,
  };
}

[[nodiscard]] inline bool sceneRangeFits(uint64_t offset, uint64_t size, size_t capacity)
{
  return offset <= static_cast<uint64_t>(capacity)
      && size <= static_cast<uint64_t>(capacity) - offset;
}

[[nodiscard]] inline SceneAssetValidationResult validateSceneAssetView(const SceneAssetView& asset)
{
  for(size_t primitiveIndex = 0; primitiveIndex < asset.primitives.size(); ++primitiveIndex) {
    const ScenePrimitive& primitive = asset.primitives[primitiveIndex];
    if(primitive.meshIndex >= asset.meshes.size() || primitive.nodeIndex >= asset.nodes.size()) {
      return {false, "scene primitive source reference is out of bounds"};
    }
    if(primitive.materialIndex >= 0
       && static_cast<size_t>(primitive.materialIndex) >= asset.materials.size()
       && !asset.materials.empty()) {
      return {false, "scene primitive material index is out of bounds"};
    }
    for(const ScenePrimitiveVariant& variant : primitive.variants) {
      if(variant.materialIndex >= 0
         && static_cast<size_t>(variant.materialIndex) >= asset.materials.size()
         && !asset.materials.empty()) {
        return {false, "scene primitive variant material index is out of bounds"};
      }
      for(const uint32_t variantIndex : variant.variantIndices) {
        if(variantIndex >= asset.materialVariantNames.size()) {
          return {false, "scene primitive variant index is out of bounds"};
        }
      }
    }
  }

  for(size_t meshIndex = 0; meshIndex < asset.meshes.size(); ++meshIndex) {
    const SceneMesh& mesh = asset.meshes[meshIndex];
    const uint64_t vertexBytes = static_cast<uint64_t>(mesh.vertexCount) * 48ull;
    const uint64_t indexBytes  = static_cast<uint64_t>(mesh.indexCount) * sizeof(uint32_t);
    if(!sceneRangeFits(mesh.vertexOffset, vertexBytes, asset.vertexPayload.size())) {
      return {false, "mesh vertex payload range is out of bounds"};
    }
    if(!sceneRangeFits(mesh.indexOffset, indexBytes, asset.indexPayload.size())) {
      return {false, "mesh index payload range is out of bounds"};
    }
    if(mesh.materialIndex >= asset.materials.size() && !asset.materials.empty()) {
      return {false, "mesh material index is out of bounds"};
    }
  }

  for(size_t textureIndex = 0; textureIndex < asset.textures.size(); ++textureIndex) {
    const SceneTexture& texture = asset.textures[textureIndex];
    if(texture.width == 0 || texture.height == 0 || texture.mipLevels == 0) {
      return {false, "texture dimensions are invalid"};
    }
    if(!sceneRangeFits(texture.payloadOffset, texture.payloadSize, asset.texturePayload.size())) {
      return {false, "texture payload range is out of bounds"};
    }
  }

  for(size_t materialIndex = 0; materialIndex < asset.materials.size(); ++materialIndex) {
    const SceneMaterial& material = asset.materials[materialIndex];
    const int32_t textureRefs[] = {
        material.baseColorTexture,
        material.normalTexture,
        material.metallicRoughnessTexture,
        material.occlusionTexture,
        material.emissiveTexture,
    };
    for(const int32_t textureRef : textureRefs) {
      if(textureRef >= 0 && static_cast<size_t>(textureRef) >= asset.textures.size()) {
        return {false, "material texture index is out of bounds"};
      }
    }
  }

  for(size_t nodeIndex = 0; nodeIndex < asset.nodes.size(); ++nodeIndex) {
    const SceneNode& node = asset.nodes[nodeIndex];
    if(node.parent >= 0 && static_cast<size_t>(node.parent) >= asset.nodes.size()) {
      return {false, "node parent index is out of bounds"};
    }
    for(const uint32_t childIndex : node.children) {
      if(childIndex >= asset.nodes.size()) {
        return {false, "node child index is out of bounds"};
      }
    }
    for(const uint32_t meshIndex : node.meshRefs) {
      if(meshIndex >= asset.meshes.size()) {
        return {false, "node mesh reference is out of bounds"};
      }
    }
  }

  for(const uint32_t rootNode : asset.rootNodes) {
    if(rootNode >= asset.nodes.size()) {
      return {false, "root node index is out of bounds"};
    }
  }

  for(const SceneLight& light : asset.lights) {
    if(light.nodeIndex >= 0 && static_cast<size_t>(light.nodeIndex) >= asset.nodes.size()) {
      return {false, "light node index is out of bounds"};
    }
  }

  for(const SceneCamera& camera : asset.cameras) {
    if(camera.nodeIndex < 0 || static_cast<size_t>(camera.nodeIndex) >= asset.nodes.size()) {
      return {false, "camera node index is out of bounds"};
    }
  }

  return {};
}

}  // namespace demo
