#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "../scene/SceneCamera.h"
#include "../scene/SceneLight.h"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// Forward declaration for tinygltf
namespace tinygltf {
class Model;
}

namespace demo {

namespace editor {
class GltfDocument;
}

struct GltfVariantBinding {
    int materialIndex = -1;
    std::vector<uint32_t> variantIndices;
};

struct GltfAnimationSamplerData {
    int inputAccessor = -1;
    int outputAccessor = -1;
    std::string interpolation;
};

struct GltfAnimationChannelData {
    int nodeIndex = -1;
    int samplerIndex = -1;
    std::string path;
};

struct GltfAnimationData {
    std::string name;
    std::vector<GltfAnimationSamplerData> samplers;
    std::vector<GltfAnimationChannelData> channels;
};

struct GltfMeshData {
    std::vector<float> positions;      // xyz (12 bytes per vertex)
    std::vector<float> normals;        // xyz (12 bytes)
    std::vector<float> texCoords;      // uv (8 bytes)
    std::vector<float> tangents;       // xyzw (16 bytes, auto-generated if missing)
    std::vector<uint32_t> indices;
    int materialIndex = -1;            // -1 = default material
    glm::mat4 transform = glm::mat4(1.0f);

    // Derived source identity. These are glTF indices, never editor handles;
    // SceneProjection resolves them against the owning GltfDocument authority.
    int documentNodeIndex = -1;
    int documentMeshIndex = -1;
    int documentPrimitiveIndex = -1;
    bool localVisible = true;
    bool effectiveVisible = true;
    std::vector<GltfVariantBinding> variantBindings;
    std::string extensionsJson;
    std::string extrasJson;
};

struct GltfMaterialData {
    // Base color
    int baseColorTexture = -1;         // -1 = no texture
    glm::vec4 baseColorFactor = glm::vec4(1.0f);

    // Metallic-Roughness (glTF convention: G=metallic, B=roughness)
    int metallicRoughnessTexture = -1;
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    int materialWorkflow = 0;          // 0 = metallic-roughness, 1 = specular-glossiness approximation

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

    std::string extensionsJson;
    std::string extrasJson;

    // Alpha mode (glTF spec)
    int alphaMode = 0;            // 0=OPAQUE, 1=MASK, 2=BLEND
    float alphaCutoff = 0.5f;     // for MASK mode
};

struct GltfImageData {
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> ktx2Data;
    int width = 0;
    int height = 0;
    int channels = 0;
    std::string uri;
    std::string mimeType;
    int fallbackImage = -1;
    bool isKtx2 = false;
    bool isSrgb = false;
    int documentImageIndex = -1;
};

// A glTF texture is a typed binding between an image and a sampler.  The
// renderer-facing SceneAsset still stores decoded image payloads, so these
// records preserve the document-level identity and the semantic image
// coordinates selected during projection without conflating texture and image
// indices.
struct GltfTextureData {
    int sourceImageIndex = -1;
    int resolvedImageIndex = -1;
    int canonicalImageIndex = -1;
    int colorImageIndex = -1;
    int linearImageIndex = -1;
    int samplerIndex = -1;
    uint8_t usage = 0;
    std::string name;
    std::string extensionsJson;
    std::string extrasJson;
};

struct GltfSamplerData {
    int magFilter = -1;
    int minFilter = -1;
    int wrapS = 10497;
    int wrapT = 10497;
    std::string name;
    std::string extensionsJson;
    std::string extrasJson;
};

struct GltfDependencyData {
    std::string relativePath;
    uint64_t fileSize = 0;
    int64_t writeTimeTicks = 0;
    uint64_t pathHash = 0;
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

    int documentNodeIndex = -1;
    bool localVisible = true;
    bool effectiveVisible = true;
    std::string extensionsJson;
    std::string extrasJson;
};

struct GltfModel {
    std::vector<GltfMeshData> meshes;
    std::vector<GltfMaterialData> materials;
    std::vector<GltfImageData> images;
    std::vector<GltfTextureData> textures;
    std::vector<GltfSamplerData> samplers;
    std::vector<GltfNodeData> nodes;
    std::vector<SceneCamera> cameras;
    std::vector<SceneLight> lights;
    std::vector<int> rootNodes;
    std::vector<std::string> materialVariantNames;
    std::vector<GltfAnimationData> animations;
    std::vector<int> cameraDocumentIndices;
    std::vector<int> lightDocumentIndices;
    std::string name;
    std::string sourcePath;
    std::string sourceDirectory;
    std::vector<GltfDependencyData> dependencies;
};

class GltfLoader {
public:
    GltfLoader() = default;
    ~GltfLoader() = default;

    bool load(const std::string& filepath, GltfModel& outModel);

    // Decode an already parsed document authority. This is the canonical
    // runtime path; it never reparses source bytes or creates another
    // mutable TinyGLTF authority.
    bool decode(const editor::GltfDocument& document, GltfModel& outModel);
    const std::string& getLastError() const { return m_lastError; }

private:
    std::string m_lastError;
    std::vector<uint8_t> m_nodeVisitState;
    std::vector<SceneLight> m_lightDefinitions;

    bool loadCandidate(const std::string& filepath, GltfModel& candidate);

    bool decodeModel(const tinygltf::Model& model,
                     const std::filesystem::path& sourcePath,
                     const editor::GltfDocument* document,
                     GltfModel& outModel);
    void processAnimations(const tinygltf::Model& model, GltfModel& outModel);

    void processLightDefinitions(const tinygltf::Model& model);
    bool processNode(const tinygltf::Model& model,
                     int nodeIndex,
                     int parentNodeIndex,
                     const glm::mat4& parentTransform,
                     bool parentVisible,
                     GltfModel& outModel);
    bool processMesh(const tinygltf::Model& model,
                     int meshIndex,
                     int nodeIndex,
                     const glm::mat4& transform,
                     bool effectiveVisible,
                     GltfModel& outModel);
    void processMaterials(const tinygltf::Model& model, GltfModel& outModel);
    void processImages(const tinygltf::Model& model, const std::filesystem::path& sourcePath, GltfModel& outModel);
};

}  // namespace demo
