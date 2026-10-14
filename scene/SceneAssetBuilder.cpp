#include "SceneAssetBuilder.h"

#include "../common/ProfilerMarkers.h"

#include <algorithm>
#include <cstring>

namespace demo {

namespace {

constexpr uint32_t kInterleavedVertexStride = 48;

void buildInterleavedVertexData(const GltfMeshData& meshData,
                                glm::vec3& outLocalBoundsMin,
                                glm::vec3& outLocalBoundsMax,
                                std::span<uint8_t> vertexData)
{
  const uint32_t vertexCount = static_cast<uint32_t>(meshData.positions.size() / 3);
  assert(vertexData.size() == static_cast<size_t>(vertexCount) * kInterleavedVertexStride);

  outLocalBoundsMin = glm::vec3(std::numeric_limits<float>::max());
  outLocalBoundsMax = glm::vec3(std::numeric_limits<float>::lowest());

  for(uint32_t i = 0; i < vertexCount; ++i) {
    float* dst = reinterpret_cast<float*>(vertexData.data() + static_cast<size_t>(i) * kInterleavedVertexStride);

    dst[0] = meshData.positions[i * 3 + 0];
    dst[1] = meshData.positions[i * 3 + 1];
    dst[2] = meshData.positions[i * 3 + 2];

    const glm::vec3 position(dst[0], dst[1], dst[2]);
    outLocalBoundsMin = glm::min(outLocalBoundsMin, position);
    outLocalBoundsMax = glm::max(outLocalBoundsMax, position);

    if(!meshData.normals.empty()) {
      dst[3] = meshData.normals[i * 3 + 0];
      dst[4] = meshData.normals[i * 3 + 1];
      dst[5] = meshData.normals[i * 3 + 2];
    } else {
      dst[3] = 0.0f;
      dst[4] = 1.0f;
      dst[5] = 0.0f;
    }

    if(!meshData.texCoords.empty()) {
      dst[6] = meshData.texCoords[i * 2 + 0];
      dst[7] = meshData.texCoords[i * 2 + 1];
    } else {
      dst[6] = 0.0f;
      dst[7] = 0.0f;
    }

    if(!meshData.tangents.empty()) {
      dst[8]  = meshData.tangents[i * 4 + 0];
      dst[9]  = meshData.tangents[i * 4 + 1];
      dst[10] = meshData.tangents[i * 4 + 2];
      dst[11] = meshData.tangents[i * 4 + 3];
    } else {
      dst[8]  = 1.0f;
      dst[9]  = 0.0f;
      dst[10] = 0.0f;
      dst[11] = 1.0f;
    }
  }
}

}  // namespace

SceneAsset SceneAssetBuilder::build(const GltfModel& model)
{
  return build(model, BuildOptions{});
}

SceneAsset SceneAssetBuilder::build(const GltfModel& model, const BuildOptions& options)
{
  VKDEMO_CPU_SCOPE("Scene.Asset.Build");
  SceneAsset asset;
  asset.name = model.name;
  asset.sourcePath = model.sourcePath;
  asset.materialVariantNames = model.materialVariantNames;
  asset.animations.reserve(model.animations.size());
  for(const GltfAnimationData& animation : model.animations) {
    SceneAnimation projected;
    projected.name = animation.name;
    projected.samplers.reserve(animation.samplers.size());
    for(const GltfAnimationSamplerData& sampler : animation.samplers) {
      projected.samplers.push_back(SceneAnimationSampler{
          .inputAccessor = sampler.inputAccessor,
          .outputAccessor = sampler.outputAccessor,
          .interpolation = sampler.interpolation,
      });
    }
    projected.channels.reserve(animation.channels.size());
    for(const GltfAnimationChannelData& channel : animation.channels) {
      projected.channels.push_back(SceneAnimationChannel{
          .documentNodeIndex = channel.nodeIndex,
          .projectedNodeIndex = -1,
          .samplerIndex = channel.samplerIndex,
          .path = channel.path,
      });
    }
    asset.animations.push_back(std::move(projected));
  }
  asset.dependencies.reserve(model.dependencies.size());
  for(const GltfDependencyData& dependency : model.dependencies) {
    asset.dependencies.push_back(SceneDependency{
        .relativePath = dependency.relativePath,
        .fileSize = dependency.fileSize,
        .writeTimeTicks = dependency.writeTimeTicks,
        .pathHash = dependency.pathHash,
    });
  }

  buildMeshes(asset, model, options);
  buildMaterials(asset, model);
  buildTextures(asset, model);
  buildNodes(asset, model);
  for(ScenePrimitive& primitive : asset.primitives) {
    const auto nodeIt = std::find_if(asset.nodes.begin(), asset.nodes.end(),
        [&primitive](const SceneNode& node) { return node.sourceNodeIndex == primitive.nodeIndex; });
    primitive.nodeIndex = nodeIt == asset.nodes.end()
        ? 0u
        : static_cast<uint32_t>(std::distance(asset.nodes.begin(), nodeIt));
  }
  asset.cameras = model.cameras;
  asset.lights = model.lights;

  for(SceneAnimation& animation : asset.animations) {
    for(SceneAnimationChannel& channel : animation.channels) {
      if(channel.documentNodeIndex < 0) {
        continue;
      }
      for(std::size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex) {
        if(model.nodes[nodeIndex].documentNodeIndex == channel.documentNodeIndex) {
          channel.projectedNodeIndex = static_cast<int32_t>(nodeIndex);
          break;
        }
      }
    }
  }

  if(options.buildMeshlets) {
    buildMeshlets(asset, model);
  }

  return asset;
}

void SceneAssetBuilder::buildMeshes(SceneAsset& asset, const GltfModel& model, const BuildOptions& options)
{
  VKDEMO_CPU_SCOPE("Scene.Asset.Build.Meshes");
  asset.meshes.reserve(model.meshes.size());

  size_t totalVertexBytes = 0;
  size_t totalIndexBytes = 0;

  // Pre-calculate sizes
  for(const GltfMeshData& gltfMesh : model.meshes) {
    const uint32_t vertexCount = static_cast<uint32_t>(gltfMesh.positions.size() / 3);
    totalVertexBytes += static_cast<size_t>(vertexCount) * kInterleavedVertexStride;
    totalIndexBytes += static_cast<size_t>(gltfMesh.indices.size()) * sizeof(uint32_t);
  }

  std::vector<uint8_t> vertexPayload(totalVertexBytes);
  std::vector<uint8_t> indexPayload(totalIndexBytes);

  size_t vertexByteOffset = 0;
  size_t indexByteOffset = 0;

  for(const GltfMeshData& gltfMesh : model.meshes) {
    SceneMesh mesh{};
    mesh.vertexOffset = static_cast<uint32_t>(vertexByteOffset);
    mesh.indexOffset  = static_cast<uint32_t>(indexByteOffset);
    mesh.vertexCount  = static_cast<uint32_t>(gltfMesh.positions.size() / 3);
    mesh.indexCount   = static_cast<uint32_t>(gltfMesh.indices.size());
    int effectiveMaterialIndex = gltfMesh.materialIndex;
    if(options.activeVariant) {
      for(const GltfVariantBinding& binding : gltfMesh.variantBindings) {
        if(std::find(binding.variantIndices.begin(), binding.variantIndices.end(),
                     *options.activeVariant) != binding.variantIndices.end()) {
          effectiveMaterialIndex = binding.materialIndex;
          break;
        }
      }
    }
    mesh.materialIndex = static_cast<uint32_t>(std::max(effectiveMaterialIndex, 0));
    mesh.exportTransform = gltfMesh.transform;
    mesh.sourceNodeIndex = gltfMesh.documentNodeIndex < 0
        ? 0u : static_cast<uint32_t>(gltfMesh.documentNodeIndex);
    mesh.sourceMeshIndex = gltfMesh.documentMeshIndex < 0
        ? 0u : static_cast<uint32_t>(gltfMesh.documentMeshIndex);
    mesh.sourcePrimitiveIndex = gltfMesh.documentPrimitiveIndex < 0
        ? 0u : static_cast<uint32_t>(gltfMesh.documentPrimitiveIndex);
    mesh.localVisible = gltfMesh.localVisible;
    mesh.effectiveVisible = gltfMesh.effectiveVisible;

    const size_t vertexDataSize = static_cast<size_t>(mesh.vertexCount) * kInterleavedVertexStride;
    const size_t indexDataSize  = static_cast<size_t>(mesh.indexCount) * sizeof(uint32_t);

    std::span<uint8_t> vertexSpan(vertexPayload.data() + vertexByteOffset, vertexDataSize);
    buildInterleavedVertexData(gltfMesh, mesh.localBoundsMin, mesh.localBoundsMax, vertexSpan);

    if(!gltfMesh.indices.empty()) {
      std::memcpy(indexPayload.data() + indexByteOffset,
                  gltfMesh.indices.data(),
                  indexDataSize);
    }

    if(effectiveMaterialIndex >= 0 && effectiveMaterialIndex < static_cast<int>(model.materials.size())) {
      const GltfMaterialData& mat = model.materials[effectiveMaterialIndex];
      mesh.alphaMode = mat.alphaMode;
      mesh.alphaCutoff = mat.alphaCutoff;
    }

    vertexByteOffset += vertexDataSize;
    indexByteOffset  += indexDataSize;

    asset.meshes.push_back(mesh);

    ScenePrimitive primitive{};
    primitive.meshIndex = static_cast<uint32_t>(asset.meshes.size() - 1u);
    primitive.nodeIndex = mesh.sourceNodeIndex;
    primitive.documentMeshIndex = mesh.sourceMeshIndex;
    primitive.documentPrimitiveIndex = mesh.sourcePrimitiveIndex;
    primitive.materialIndex = effectiveMaterialIndex;
    primitive.localVisible = gltfMesh.localVisible;
    primitive.effectiveVisible = gltfMesh.effectiveVisible;
    primitive.extensionsJson = gltfMesh.extensionsJson;
    primitive.extrasJson = gltfMesh.extrasJson;
    for(const GltfVariantBinding& binding : gltfMesh.variantBindings) {
      primitive.variants.push_back(ScenePrimitiveVariant{
          .materialIndex = binding.materialIndex,
          .variantIndices = binding.variantIndices,
      });
    }
    asset.primitives.push_back(std::move(primitive));
  }
  asset.vertexPayload = std::move(vertexPayload);
  asset.indexPayload = std::move(indexPayload);
}

void SceneAssetBuilder::buildMaterials(SceneAsset& asset, const GltfModel& model)
{
  VKDEMO_CPU_SCOPE("Scene.Asset.Build.Materials");
  asset.materials.reserve(model.materials.size());

  for(const GltfMaterialData& gltfMat : model.materials) {
    SceneMaterial mat{};
    mat.baseColorFactor    = gltfMat.baseColorFactor;
    mat.metallicFactor     = gltfMat.metallicFactor;
    mat.roughnessFactor    = gltfMat.roughnessFactor;
    mat.normalScale        = gltfMat.normalScale;
    mat.occlusionStrength  = gltfMat.occlusionStrength;
    mat.emissiveFactor     = gltfMat.emissiveFactor;
    mat.baseColorTexture           = gltfMat.baseColorTexture;
    mat.normalTexture              = gltfMat.normalTexture;
    mat.metallicRoughnessTexture   = gltfMat.metallicRoughnessTexture;
    mat.occlusionTexture           = gltfMat.occlusionTexture;
    mat.emissiveTexture            = gltfMat.emissiveTexture;
    mat.alphaMode          = gltfMat.alphaMode;
    mat.alphaCutoff        = gltfMat.alphaCutoff;
    mat.materialWorkflow   = gltfMat.materialWorkflow;
    mat.doubleSided        = gltfMat.doubleSided;
    mat.name               = gltfMat.name;
    mat.extensionsJson     = gltfMat.extensionsJson;
    mat.extrasJson         = gltfMat.extrasJson;

    asset.materials.push_back(mat);
  }
}

void SceneAssetBuilder::buildTextures(SceneAsset& asset, const GltfModel& model)
{
  VKDEMO_CPU_SCOPE("Scene.Asset.Build.Textures");
  asset.textures.reserve(model.images.size());
  std::vector<uint8_t> texturePayload;

  size_t payloadOffset = 0;

  for(const GltfImageData& gltfImage : model.images) {
    SceneTexture tex{};
    tex.width  = static_cast<uint32_t>(gltfImage.width);
    tex.height = static_cast<uint32_t>(gltfImage.height);
    tex.format = gltfImage.isSrgb
        ? rhi::TextureFormat::rgba8Srgb
        : rhi::TextureFormat::rgba8Unorm;  // KTX2 path overrides in upload
    tex.isKtx2 = gltfImage.isKtx2;
    tex.uri    = gltfImage.uri;
    if(gltfImage.documentImageIndex >= 0) {
      tex.sourceImageIndex = static_cast<uint32_t>(gltfImage.documentImageIndex);
    }

    const std::vector<uint8_t>* sourceData = nullptr;
    if(gltfImage.isKtx2 && !gltfImage.ktx2Data.empty()) {
      sourceData = &gltfImage.ktx2Data;
      // Format will be resolved at upload time by Ktx2Loader
    } else if(!gltfImage.pixels.empty()) {
      sourceData = &gltfImage.pixels;
    }

    if(sourceData) {
      tex.payloadOffset = payloadOffset;
      tex.payloadSize   = sourceData->size();
      texturePayload.insert(texturePayload.end(), sourceData->begin(), sourceData->end());
      payloadOffset += sourceData->size();
    } else {
      tex.payloadOffset = 0;
      tex.payloadSize   = 0;
    }

    asset.textures.push_back(tex);
  }
  asset.texturePayload = std::move(texturePayload);
}

void SceneAssetBuilder::buildNodes(SceneAsset& asset, const GltfModel& model)
{
  VKDEMO_CPU_SCOPE("Scene.Asset.Build.Nodes");
  asset.nodes.reserve(model.nodes.size());
  asset.rootNodes.clear();
  asset.rootNodes.reserve(model.rootNodes.size());
  for(const int rootNodeIndex : model.rootNodes) {
    if(rootNodeIndex >= 0) {
      asset.rootNodes.push_back(static_cast<uint32_t>(rootNodeIndex));
    }
  }

  for(const GltfNodeData& gltfNode : model.nodes) {
    SceneNode node{};
    node.name        = gltfNode.name;
    node.parent      = gltfNode.parent;
    node.children.reserve(gltfNode.children.size());
    for(const int childIndex : gltfNode.children) {
      if(childIndex >= 0) {
        node.children.push_back(static_cast<uint32_t>(childIndex));
      }
    }
    node.translation = gltfNode.translation;
    node.rotation    = gltfNode.rotation;
    node.scale       = gltfNode.scale;
    node.localTransform = gltfNode.localTransform;
    node.worldTransform = gltfNode.worldTransform;
    node.sourceNodeIndex = gltfNode.documentNodeIndex < 0
        ? 0u : static_cast<uint32_t>(gltfNode.documentNodeIndex);
    node.localVisible = gltfNode.localVisible;
    node.effectiveVisible = gltfNode.effectiveVisible;
    node.extensionsJson = gltfNode.extensionsJson;
    node.extrasJson = gltfNode.extrasJson;

    // Build meshRefs from firstMeshIndex + meshCount
    const uint32_t meshEnd = gltfNode.firstMeshIndex + gltfNode.meshCount;
    for(uint32_t mi = gltfNode.firstMeshIndex; mi < meshEnd; ++mi) {
      if(mi < static_cast<uint32_t>(model.meshes.size())) {
        node.meshRefs.push_back(mi);
      }
    }

    asset.nodes.push_back(node);
  }
}

void SceneAssetBuilder::buildMeshlets(SceneAsset& asset, const GltfModel& model)
{
  VKDEMO_CPU_SCOPE("Scene.Asset.Build.Meshlets");
  // TODO: integrate MeshletConverter when meshlet path is needed
  // For now, leave meshletPayloads empty; the GPUDrivenRenderer can fall back
  // to runtime conversion from GltfModel or from SceneAsset's vertexPayload.
}

}  // namespace demo
