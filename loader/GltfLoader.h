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
    std::vector<float> positions;      // x,y,z per vertex
    std::vector<float> normals;        // x,y,z per vertex
    std::vector<float> texCoords;      // u,v per vertex
    std::vector<uint32_t> indices;
    int materialIndex = -1;            // -1 = default material
    glm::mat4 transform = glm::mat4(1.0f);
};

struct GltfMaterialData {
    int baseColorTexture = -1;         // -1 = no texture
    glm::vec4 baseColorFactor = glm::vec4(1.0f);
};

struct GltfImageData {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    int channels = 0;
};

struct GltfModel {
    std::vector<GltfMeshData> meshes;
    std::vector<GltfMaterialData> materials;
    std::vector<GltfImageData> images;
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

    bool processNode(const tinygltf::Model& model, int nodeIndex, const glm::mat4& parentTransform, GltfModel& outModel);
    bool processMesh(const tinygltf::Model& model, int meshIndex, const glm::mat4& transform, GltfModel& outModel);
    void processMaterials(const tinygltf::Model& model, GltfModel& outModel);
    void processImages(const tinygltf::Model& model, GltfModel& outModel);
};

}  // namespace demo