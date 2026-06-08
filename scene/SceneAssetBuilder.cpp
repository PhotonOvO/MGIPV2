#include "SceneAssetBuilder.h"

#include "../render/MeshPool.h"
#include "../render/MeshletConverter.h"

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
  ASSERT(vertexData.size() == static_cast<size_t>(vertexCount) * kInterleavedVertexStride,
         "Interleaved vertex buffer size mismatch");

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

SceneAsset SceneAssetBuilder::build(const GltfModel& model, const BuildOptions& options)
{
  SceneAsset asset;
  asset.name = model.name;
  asset.sourcePath = model.sourcePath;
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
  asset.lights = model.lights;

  if(options.buildMeshlets) {
    buildMeshlets(asset, model);
  }

  return asset;
}

void SceneAssetBuilder::buildMeshes(SceneAsset& asset, const GltfModel& model, const BuildOptions& options)
{
  asset.meshes.reserve(model.meshes.size());

  size_t totalVertexBytes = 0;
  size_t totalIndexBytes = 0;

  // Pre-calculate sizes
  for(const GltfMeshData& gltfMesh : model.meshes) {
    const uint32_t vertexCount = static_cast<uint32_t>(gltfMesh.positions.size() / 3);
    totalVertexBytes += static_cast<size_t>(vertexCount) * kInterleavedVertexStride;
    totalIndexBytes += static_cast<size_t>(gltfMesh.indices.size()) * sizeof(uint32_t);
  }

  asset.vertexPayload.resize(totalVertexBytes);
  asset.indexPayload.resize(totalIndexBytes);

  size_t vertexByteOffset = 0;
  size_t indexByteOffset = 0;

  for(const GltfMeshData& gltfMesh : model.meshes) {
    SceneMesh mesh{};
    mesh.vertexOffset = static_cast<uint32_t>(vertexByteOffset);
    mesh.indexOffset  = static_cast<uint32_t>(indexByteOffset);
    mesh.vertexCount  = static_cast<uint32_t>(gltfMesh.positions.size() / 3);
    mesh.indexCount   = static_cast<uint32_t>(gltfMesh.indices.size());
    mesh.materialIndex = static_cast<uint32_t>(std::max(gltfMesh.materialIndex, 0));
    mesh.exportTransform = gltfMesh.transform;

    const size_t vertexDataSize = static_cast<size_t>(mesh.vertexCount) * kInterleavedVertexStride;
    const size_t indexDataSize  = static_cast<size_t>(mesh.indexCount) * sizeof(uint32_t);

    std::span<uint8_t> vertexSpan(asset.vertexPayload.data() + vertexByteOffset, vertexDataSize);
    buildInterleavedVertexData(gltfMesh, mesh.localBoundsMin, mesh.localBoundsMax, vertexSpan);

    if(!gltfMesh.indices.empty()) {
      std::memcpy(asset.indexPayload.data() + indexByteOffset,
                  gltfMesh.indices.data(),
                  indexDataSize);
    }

    if(gltfMesh.materialIndex >= 0 && gltfMesh.materialIndex < static_cast<int>(model.materials.size())) {
      const GltfMaterialData& mat = model.materials[gltfMesh.materialIndex];
      mesh.alphaMode = mat.alphaMode;
      mesh.alphaCutoff = mat.alphaCutoff;
    }

    vertexByteOffset += vertexDataSize;
    indexByteOffset  += indexDataSize;

    asset.meshes.push_back(mesh);
  }
}

void SceneAssetBuilder::buildMaterials(SceneAsset& asset, const GltfModel& model)
{
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

    asset.materials.push_back(mat);
  }
}

void SceneAssetBuilder::buildTextures(SceneAsset& asset, const GltfModel& model)
{
  asset.textures.reserve(model.images.size());
  asset.texturePayload.clear();

  size_t payloadOffset = 0;

  for(const GltfImageData& gltfImage : model.images) {
    SceneTexture tex{};
    tex.width  = static_cast<uint32_t>(gltfImage.width);
    tex.height = static_cast<uint32_t>(gltfImage.height);
    tex.format = rhi::TextureFormat::rgba8Unorm;  // Default; KTX2 path overrides in upload
    tex.isKtx2 = gltfImage.isKtx2;
    tex.uri    = gltfImage.uri;

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
      asset.texturePayload.insert(asset.texturePayload.end(), sourceData->begin(), sourceData->end());
      payloadOffset += sourceData->size();
    } else {
      tex.payloadOffset = 0;
      tex.payloadSize   = 0;
    }

    asset.textures.push_back(tex);
  }
}

void SceneAssetBuilder::buildNodes(SceneAsset& asset, const GltfModel& model)
{
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
  // TODO: integrate MeshletConverter when meshlet path is needed
  // For now, leave meshletPayloads empty; the GPUDrivenRenderer can fall back
  // to runtime conversion from GltfModel or from SceneAsset's vertexPayload.
}

}  // namespace demo
