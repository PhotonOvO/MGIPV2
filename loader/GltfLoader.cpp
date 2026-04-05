#include "GltfLoader.h"

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tiny_gltf.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <iostream>

namespace demo {

// Forward declarations for tangent generation
static std::vector<float> computeTangents(
    const std::vector<float>& positions,
    const std::vector<float>& normals,
    const std::vector<float>& texCoords,
    const std::vector<uint32_t>& indices
);
static void generateTangentsIfMissing(GltfMeshData& mesh);

bool GltfLoader::load(const std::string& filepath, GltfModel& outModel) {
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err;
    std::string warn;

    // Determine file type based on extension
    bool binary = false;
    if (filepath.size() >= 4) {
        std::string ext = filepath.substr(filepath.size() - 4);
        if (ext == ".glb") {
            binary = true;
        }
    }

    // Load the glTF file
    bool success = false;
    if (binary) {
        success = loader.LoadBinaryFromFile(&model, &err, &warn, filepath);
    } else {
        success = loader.LoadASCIIFromFile(&model, &err, &warn, filepath);
    }

    if (!err.empty()) {
        m_lastError = "glTF loading error: " + err;
        return false;
    }

    if (!success) {
        m_lastError = "Failed to load glTF file: " + filepath;
        return false;
    }

    // Process materials and images first (they are referenced by meshes)
    processMaterials(model, outModel);
    processImages(model, outModel);

    // Set model name from file
    size_t lastSlash = filepath.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        outModel.name = filepath.substr(lastSlash + 1);
    } else {
        outModel.name = filepath;
    }

    // Find the default scene or use the first scene
    int sceneIndex = model.defaultScene;
    if (sceneIndex < 0 && !model.scenes.empty()) {
        sceneIndex = 0;
    }

    if (sceneIndex >= 0 && sceneIndex < static_cast<int>(model.scenes.size())) {
        const auto& scene = model.scenes[sceneIndex];

        // Traverse all root nodes in the scene
        for (int nodeIndex : scene.nodes) {
            if (!processNode(model, nodeIndex, glm::mat4(1.0f), outModel)) {
                return false;
            }
        }
    } else {
        // No scenes - try processing all nodes directly
        for (int i = 0; i < static_cast<int>(model.nodes.size()); ++i) {
            if (!processNode(model, i, glm::mat4(1.0f), outModel)) {
                return false;
            }
        }
    }

    return true;
}

bool GltfLoader::processNode(const tinygltf::Model& model, int nodeIndex,
                              const glm::mat4& parentTransform, GltfModel& outModel) {
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size())) {
        m_lastError = "Invalid node index: " + std::to_string(nodeIndex);
        return false;
    }

    const auto& node = model.nodes[nodeIndex];

    // Compute local transform
    glm::mat4 localTransform(1.0f);

    if (node.matrix.size() == 16) {
        // Use matrix if provided
        localTransform = glm::make_mat4(node.matrix.data());
    } else {
        // Build from TRS components
        glm::vec3 translation(0.0f);
        glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 scale(1.0f);

        if (node.translation.size() == 3) {
            translation = glm::vec3(node.translation[0], node.translation[1], node.translation[2]);
        }

        if (node.rotation.size() == 4) {
            // glTF uses (x, y, z, w) quaternion format
            rotation = glm::quat(
                static_cast<float>(node.rotation[3]),  // w
                static_cast<float>(node.rotation[0]),  // x
                static_cast<float>(node.rotation[1]),  // y
                static_cast<float>(node.rotation[2])   // z
            );
        }

        if (node.scale.size() == 3) {
            scale = glm::vec3(node.scale[0], node.scale[1], node.scale[2]);
        }

        // Compose TRS: T * R * S
        glm::mat4 t = glm::translate(glm::mat4(1.0f), translation);
        glm::mat4 r = glm::mat4_cast(rotation);
        glm::mat4 s = glm::scale(glm::mat4(1.0f), scale);
        localTransform = t * r * s;
    }

    glm::mat4 globalTransform = parentTransform * localTransform;

    // Process mesh if present
    if (node.mesh >= 0) {
        if (!processMesh(model, node.mesh, globalTransform, outModel)) {
            return false;
        }
    }

    // Recursively process children
    for (int childIndex : node.children) {
        if (!processNode(model, childIndex, globalTransform, outModel)) {
            return false;
        }
    }

    return true;
}

bool GltfLoader::processMesh(const tinygltf::Model& model, int meshIndex,
                              const glm::mat4& transform, GltfModel& outModel) {
    if (meshIndex < 0 || meshIndex >= static_cast<int>(model.meshes.size())) {
        m_lastError = "Invalid mesh index: " + std::to_string(meshIndex);
        return false;
    }

    const auto& mesh = model.meshes[meshIndex];

    // Process each primitive
    for (const auto& primitive : mesh.primitives) {
        GltfMeshData meshData;
        meshData.transform = transform;
        meshData.materialIndex = primitive.material;

        // Get positions (required)
        auto posIt = primitive.attributes.find("POSITION");
        if (posIt == primitive.attributes.end()) {
            m_lastError = "Mesh primitive missing POSITION attribute";
            return false;
        }

        const auto& posAccessor = model.accessors[posIt->second];
        const auto& posBufferView = model.bufferViews[posAccessor.bufferView];
        const auto& posBuffer = model.buffers[posBufferView.buffer];

        // Extract positions
        const float* posData = reinterpret_cast<const float*>(
            &posBuffer.data[posBufferView.byteOffset + posAccessor.byteOffset]
        );

        size_t vertexCount = posAccessor.count;
        meshData.positions.reserve(vertexCount * 3);

        for (size_t i = 0; i < vertexCount; ++i) {
            meshData.positions.push_back(posData[i * 3]);
            meshData.positions.push_back(posData[i * 3 + 1]);
            meshData.positions.push_back(posData[i * 3 + 2]);
        }

        // Get normals (optional)
        auto normIt = primitive.attributes.find("NORMAL");
        if (normIt != primitive.attributes.end()) {
            const auto& normAccessor = model.accessors[normIt->second];
            const auto& normBufferView = model.bufferViews[normAccessor.bufferView];
            const auto& normBuffer = model.buffers[normBufferView.buffer];

            const float* normData = reinterpret_cast<const float*>(
                &normBuffer.data[normBufferView.byteOffset + normAccessor.byteOffset]
            );

            meshData.normals.reserve(vertexCount * 3);
            for (size_t i = 0; i < vertexCount; ++i) {
                meshData.normals.push_back(normData[i * 3]);
                meshData.normals.push_back(normData[i * 3 + 1]);
                meshData.normals.push_back(normData[i * 3 + 2]);
            }
        } else {
            // Generate default normals (facing up)
            meshData.normals.resize(vertexCount * 3, 0.0f);
            for (size_t i = 0; i < vertexCount; ++i) {
                meshData.normals[i * 3 + 1] = 1.0f;  // Y-up
            }
        }

        // Get texture coordinates (optional)
        auto texIt = primitive.attributes.find("TEXCOORD_0");
        if (texIt != primitive.attributes.end()) {
            const auto& texAccessor = model.accessors[texIt->second];
            const auto& texBufferView = model.bufferViews[texAccessor.bufferView];
            const auto& texBuffer = model.buffers[texBufferView.buffer];

            const float* texData = reinterpret_cast<const float*>(
                &texBuffer.data[texBufferView.byteOffset + texAccessor.byteOffset]
            );

            meshData.texCoords.reserve(vertexCount * 2);
            for (size_t i = 0; i < vertexCount; ++i) {
                // glTF 2.0 uses upper-left origin (same as Vulkan), no Y-flip needed
                meshData.texCoords.push_back(texData[i * 2]);
                meshData.texCoords.push_back(texData[i * 2 + 1]);
            }
        } else {
            // Generate default UVs
            meshData.texCoords.resize(vertexCount * 2, 0.0f);
        }

        // Get indices
        if (primitive.indices >= 0) {
            const auto& indexAccessor = model.accessors[primitive.indices];
            const auto& indexBufferView = model.bufferViews[indexAccessor.bufferView];
            const auto& indexBuffer = model.buffers[indexBufferView.buffer];

            meshData.indices.reserve(indexAccessor.count);

            // Handle different index component types
            const uint8_t* indexData = &indexBuffer.data[indexBufferView.byteOffset + indexAccessor.byteOffset];

            switch (indexAccessor.componentType) {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
                    const uint8_t* indices = reinterpret_cast<const uint8_t*>(indexData);
                    for (size_t i = 0; i < indexAccessor.count; ++i) {
                        meshData.indices.push_back(static_cast<uint32_t>(indices[i]));
                    }
                    break;
                }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                    const uint16_t* indices = reinterpret_cast<const uint16_t*>(indexData);
                    for (size_t i = 0; i < indexAccessor.count; ++i) {
                        meshData.indices.push_back(static_cast<uint32_t>(indices[i]));
                    }
                    break;
                }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
                    const uint32_t* indices = reinterpret_cast<const uint32_t*>(indexData);
                    for (size_t i = 0; i < indexAccessor.count; ++i) {
                        meshData.indices.push_back(indices[i]);
                    }
                    break;
                }
                default:
                    m_lastError = "Unsupported index component type: " +
                                  std::to_string(indexAccessor.componentType);
                    return false;
            }
        } else {
            // No indices provided - generate sequential indices
            meshData.indices.reserve(vertexCount);
            for (size_t i = 0; i < vertexCount; ++i) {
                meshData.indices.push_back(static_cast<uint32_t>(i));
            }
        }

        // Generate tangents if not provided by the glTF file
        generateTangentsIfMissing(meshData);

        outModel.meshes.push_back(std::move(meshData));
    }

    return true;
}

void GltfLoader::processMaterials(const tinygltf::Model& model, GltfModel& outModel) {
    outModel.materials.reserve(model.materials.size());

    for (const auto& mat : model.materials) {
        GltfMaterialData data;
        data.name = mat.name;
        data.doubleSided = mat.doubleSided;

        // PBR Metallic-Roughness
        const auto& pbr = mat.pbrMetallicRoughness;

        // Base color
        data.baseColorFactor = glm::vec4(
            static_cast<float>(pbr.baseColorFactor[0]),
            static_cast<float>(pbr.baseColorFactor[1]),
            static_cast<float>(pbr.baseColorFactor[2]),
            static_cast<float>(pbr.baseColorFactor[3])
        );
        if (pbr.baseColorTexture.index >= 0) {
            data.baseColorTexture = pbr.baseColorTexture.index;
        }

        // Metallic-Roughness
        data.metallicFactor = static_cast<float>(pbr.metallicFactor);
        data.roughnessFactor = static_cast<float>(pbr.roughnessFactor);
        if (pbr.metallicRoughnessTexture.index >= 0) {
            data.metallicRoughnessTexture = pbr.metallicRoughnessTexture.index;
        }

        // Normal map
        if (mat.normalTexture.index >= 0) {
            data.normalTexture = mat.normalTexture.index;
            data.normalScale = static_cast<float>(mat.normalTexture.scale);
        }

        // Occlusion
        if (mat.occlusionTexture.index >= 0) {
            data.occlusionTexture = mat.occlusionTexture.index;
            data.occlusionStrength = static_cast<float>(mat.occlusionTexture.strength);
        }

        // Emissive
        data.emissiveFactor = glm::vec3(
            static_cast<float>(mat.emissiveFactor[0]),
            static_cast<float>(mat.emissiveFactor[1]),
            static_cast<float>(mat.emissiveFactor[2])
        );
        if (mat.emissiveTexture.index >= 0) {
            data.emissiveTexture = mat.emissiveTexture.index;
        }

        // Alpha mode (glTF spec)
        std::string alphaModeStr = mat.alphaMode;
        if (alphaModeStr == "MASK") {
            data.alphaMode = 1;  // LAlphaMask
            data.alphaCutoff = static_cast<float>(mat.alphaCutoff);
        } else if (alphaModeStr == "BLEND") {
            data.alphaMode = 2;  // LAlphaBlend
        } else {
            data.alphaMode = 0;  // LAlphaOpaque (default)
        }

        outModel.materials.push_back(data);
    }
}

void GltfLoader::processImages(const tinygltf::Model& model, GltfModel& outModel) {
    outModel.images.reserve(model.images.size());

    for (const auto& image : model.images) {
        GltfImageData imageData;
        imageData.width = image.width;
        imageData.height = image.height;
        imageData.channels = image.component;

        // Copy pixel data directly from tinygltf
        imageData.pixels = image.image;

        outModel.images.push_back(std::move(imageData));
    }
}

// Tangent generation using Lengyel's method
static std::vector<float> computeTangents(
    const std::vector<float>& positions,
    const std::vector<float>& normals,
    const std::vector<float>& texCoords,
    const std::vector<uint32_t>& indices
) {
    const size_t vertexCount = positions.size() / 3;
    std::vector<glm::vec3> tangents(vertexCount, glm::vec3(0.0f));
    std::vector<glm::vec3> bitangents(vertexCount, glm::vec3(0.0f));

    // Accumulate tangent vectors for each triangle
    for (size_t i = 0; i < indices.size(); i += 3) {
        const uint32_t i0 = indices[i];
        const uint32_t i1 = indices[i + 1];
        const uint32_t i2 = indices[i + 2];

        const glm::vec3 p0(positions[i0 * 3], positions[i0 * 3 + 1], positions[i0 * 3 + 2]);
        const glm::vec3 p1(positions[i1 * 3], positions[i1 * 3 + 1], positions[i1 * 3 + 2]);
        const glm::vec3 p2(positions[i2 * 3], positions[i2 * 3 + 1], positions[i2 * 3 + 2]);

        const glm::vec2 uv0(texCoords[i0 * 2], texCoords[i0 * 2 + 1]);
        const glm::vec2 uv1(texCoords[i1 * 2], texCoords[i1 * 2 + 1]);
        const glm::vec2 uv2(texCoords[i2 * 2], texCoords[i2 * 2 + 1]);

        const glm::vec3 e1 = p1 - p0;
        const glm::vec3 e2 = p2 - p0;
        const glm::vec2 duv1 = uv1 - uv0;
        const glm::vec2 duv2 = uv2 - uv0;

        const float r = 1.0f / (duv1.x * duv2.y - duv2.x * duv1.y);

        const glm::vec3 tangent = r * (e1 * duv2.y - e2 * duv1.y);
        const glm::vec3 bitangent = r * (e2 * duv1.x - e1 * duv2.x);

        tangents[i0] += tangent;
        tangents[i1] += tangent;
        tangents[i2] += tangent;

        bitangents[i0] += bitangent;
        bitangents[i1] += bitangent;
        bitangents[i2] += bitangent;
    }

    // Orthogonalize and compute handedness
    std::vector<float> result;
    result.reserve(vertexCount * 4);

    for (size_t i = 0; i < vertexCount; ++i) {
        const glm::vec3 n(normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2]);
        glm::vec3 t = tangents[i] - n * glm::dot(n, tangents[i]);
        t = glm::normalize(t);

        // Handedness (w component)
        const float w = (glm::dot(glm::cross(n, t), bitangents[i]) < 0.0f) ? -1.0f : 1.0f;

        result.push_back(t.x);
        result.push_back(t.y);
        result.push_back(t.z);
        result.push_back(w);
    }

    return result;
}

static void generateTangentsIfMissing(GltfMeshData& mesh) {
    if (mesh.tangents.empty() && !mesh.normals.empty() && !mesh.texCoords.empty() && !mesh.indices.empty()) {
        mesh.tangents = computeTangents(mesh.positions, mesh.normals, mesh.texCoords, mesh.indices);
    }
}

}  // namespace demo