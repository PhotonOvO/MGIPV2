#pragma once

#include <cstdint>
#include <string>
#include <vector>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// Forward declaration for tinygltf
namespace tinygltf {
struct Model;
}

namespace demo {

struct GltfMeshData {
    std::vector<float> positions;      // xyz (12 bytes per vertex)
    std::vector<float> normals;        // xyz (12 bytes)
    std::vector<float> texCoords;      // uv (8 bytes)
    std::vector<float> tangents;       // xyzw (16 bytes, auto-generated if missing)
    std::vector<uint32_t> indices;
    int materialIndex = -1;            // -1 = default material
    glm::mat4 transform = glm::mat4(1.0f);
};

struct GltfMaterialData {
    // Base color
    int baseColorTexture = -1;         // -1 = no texture
    glm::vec4 baseColorFactor = glm::vec4(1.0f);

    // Metallic-Roughness (glTF convention: G=metallic, B=roughness)
    int metallicRoughnessTexture = -1;
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;

    // Normal map
    int normalTexture = -1;
    float normalScale = 1.0f;

    // Occlusion
    int occlusionTexture = -1;
    float occlusionStrength = 1.0f;

    // Emissive
    int emissiveTexture = -1;
    glm::vec3 emissiveFactor = glm::vec3(0.0f);

    // Additional flags
    bool doubleSided = false;
    std::string name;

    // Alpha mode (glTF spec)
    int alphaMode = 0;            // 0=OPAQUE, 1=MASK, 2=BLEND
    float alphaCutoff = 0.5f;     // for MASK mode
};

struct GltfImageData {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    int channels = 0;
};

struct GltfNodeData {
    std::string name;
    int parent = -1;
    std::vector<int> children;
    glm::vec3 translation = glm::vec3(0.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 rotationEulerDegrees = glm::vec3(0.0f);
    glm::vec3 scale = glm::vec3(1.0f);
    glm::mat4 localTransform = glm::mat4(1.0f);
    glm::mat4 worldTransform = glm::mat4(1.0f);
    uint32_t firstMeshIndex = 0;
    uint32_t meshCount = 0;
};

struct GltfModel {
    std::vector<GltfMeshData> meshes;
    std::vector<GltfMaterialData> materials;
    std::vector<GltfImageData> images;
    std::vector<GltfNodeData> nodes;
    std::vector<int> rootNodes;
    std::string name;
};

class GltfLoader {
public:
    GltfLoader() = default;
    ~GltfLoader() = default;

    bool load(const std::string& filepath, GltfModel& outModel);
    const std::string& getLastError() const { return m_lastError; }

private:
    std::string m_lastError;

    bool processNode(const tinygltf::Model& model,
                     int nodeIndex,
                     int parentNodeIndex,
                     const glm::mat4& parentTransform,
                     GltfModel& outModel);
    bool processMesh(const tinygltf::Model& model, int meshIndex, const glm::mat4& transform, GltfModel& outModel);
    void processMaterials(const tinygltf::Model& model, GltfModel& outModel);
    void processImages(const tinygltf::Model& model, GltfModel& outModel);
};

}  // namespace demo
