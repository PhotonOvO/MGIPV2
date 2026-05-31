#include "GltfLoader.h"

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tiny_gltf.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>

namespace demo {

static int resolveTextureSourceIndex(const tinygltf::Model& model, int textureIndex);

namespace {

constexpr size_t kMaxReasonableNodes = 1u << 16;
constexpr size_t kMaxReasonableMeshes = 1u << 16;
constexpr size_t kMaxReasonableMaterials = 1u << 14;
constexpr size_t kMaxReasonableImages = 1u << 14;
constexpr size_t kMaxReasonableImageDimension = 1u << 14;
constexpr size_t kMaxReasonableImageBytes = 1u << 28;
constexpr size_t kMaxReasonableAccessorElements = 1u << 24;
// Bistro-scale scenes legitimately exceed a 512 MiB decoded payload once
// textures are expanded to RGBA8, so keep a higher but still explicit cap.
constexpr uint64_t kMaxReasonableModelPayloadBytes = 4ull << 30;
constexpr std::array<uint8_t, 12> kKtx2Identifier{
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

bool hasReasonableModelShape(const tinygltf::Model& model)
{
    return model.nodes.size() <= kMaxReasonableNodes
        && model.meshes.size() <= kMaxReasonableMeshes
        && model.materials.size() <= kMaxReasonableMaterials
        && model.images.size() <= kMaxReasonableImages;
}

uint64_t estimateModelPayloadBytes(const GltfModel& model)
{
    uint64_t totalBytes = 0;

    for(const GltfMeshData& mesh : model.meshes)
    {
        totalBytes += static_cast<uint64_t>(mesh.positions.size()) * sizeof(float);
        totalBytes += static_cast<uint64_t>(mesh.normals.size()) * sizeof(float);
        totalBytes += static_cast<uint64_t>(mesh.texCoords.size()) * sizeof(float);
        totalBytes += static_cast<uint64_t>(mesh.tangents.size()) * sizeof(float);
        totalBytes += static_cast<uint64_t>(mesh.indices.size()) * sizeof(uint32_t);
    }

    for(const GltfImageData& image : model.images)
    {
        totalBytes += static_cast<uint64_t>(image.pixels.size());
    }

    for(const GltfNodeData& node : model.nodes)
    {
        totalBytes += static_cast<uint64_t>(node.children.size()) * sizeof(int);
    }

    return totalBytes;
}

glm::mat4 composeTransform(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale)
{
    return glm::translate(glm::mat4(1.0f), translation)
         * glm::mat4_cast(rotation)
         * glm::scale(glm::mat4(1.0f), scale);
}

glm::vec3 sanitizeEulerDegrees(const glm::vec3& radians)
{
    const glm::vec3 degrees = glm::degrees(radians);
    return glm::vec3(
        std::isfinite(degrees.x) ? degrees.x : 0.0f,
        std::isfinite(degrees.y) ? degrees.y : 0.0f,
        std::isfinite(degrees.z) ? degrees.z : 0.0f);
}

std::vector<uint8_t> expandToRgba8(const std::vector<uint8_t>& pixels, int width, int height, int channels)
{
    if(width <= 0 || height <= 0 || pixels.empty())
    {
        return {};
    }

    if(channels == 4)
    {
        return pixels;
    }

    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<uint8_t> rgba(pixelCount * 4u, 255u);

    for(size_t i = 0; i < pixelCount; ++i)
    {
        const size_t srcIndex = i * static_cast<size_t>(std::max(channels, 1));
        const size_t dstIndex = i * 4u;

        switch(channels)
        {
            case 1:
                rgba[dstIndex + 0] = pixels[srcIndex + 0];
                rgba[dstIndex + 1] = pixels[srcIndex + 0];
                rgba[dstIndex + 2] = pixels[srcIndex + 0];
                break;
            case 2:
                rgba[dstIndex + 0] = pixels[srcIndex + 0];
                rgba[dstIndex + 1] = pixels[srcIndex + 0];
                rgba[dstIndex + 2] = pixels[srcIndex + 0];
                rgba[dstIndex + 3] = pixels[srcIndex + 1];
                break;
            case 3:
                rgba[dstIndex + 0] = pixels[srcIndex + 0];
                rgba[dstIndex + 1] = pixels[srcIndex + 1];
                rgba[dstIndex + 2] = pixels[srcIndex + 2];
                break;
            default:
                rgba[dstIndex + 0] = pixels[srcIndex + 0];
                rgba[dstIndex + 1] = pixels[srcIndex + 1];
                rgba[dstIndex + 2] = pixels[srcIndex + 2];
                rgba[dstIndex + 3] = pixels[srcIndex + 3];
                break;
        }
    }

    return rgba;
}

bool hasExtension(const std::filesystem::path& path, const char* extension)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == extension;
}

uint64_t hashPathForDependency(const std::filesystem::path& path)
{
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    const std::filesystem::path hashPath = ec ? path.lexically_normal() : canonical;
    return static_cast<uint64_t>(std::hash<std::string>{}(hashPath.generic_string()));
}

void addExternalDependency(const std::filesystem::path& sourceDirectory,
                           const std::filesystem::path& relativePath,
                           GltfModel& outModel)
{
    if(relativePath.empty() || relativePath.is_absolute()) {
        return;
    }
    const std::string relativeGeneric = relativePath.generic_string();
    if(relativeGeneric.rfind("data:", 0) == 0) {
        return;
    }

    const std::filesystem::path dependencyPath = sourceDirectory / relativePath;
    std::error_code ec;
    if(!std::filesystem::exists(dependencyPath, ec) || ec) {
        return;
    }
    if(!std::filesystem::is_regular_file(dependencyPath, ec) || ec) {
        return;
    }

    const uint64_t pathHash = hashPathForDependency(dependencyPath);
    for(const GltfDependencyData& dependency : outModel.dependencies) {
        if(dependency.pathHash == pathHash) {
            return;
        }
    }

    std::error_code sizeEc;
    std::error_code timeEc;
    const uint64_t fileSize = std::filesystem::file_size(dependencyPath, sizeEc);
    const auto writeTime = std::filesystem::last_write_time(dependencyPath, timeEc);
    outModel.dependencies.push_back(GltfDependencyData{
        .relativePath = relativeGeneric,
        .fileSize = sizeEc ? 0 : fileSize,
        .writeTimeTicks = timeEc ? 0 : static_cast<int64_t>(writeTime.time_since_epoch().count()),
        .pathHash = pathHash,
    });
}

bool hasKtx2Identifier(const unsigned char* bytes, int size)
{
    return bytes != nullptr
        && size >= static_cast<int>(kKtx2Identifier.size())
        && std::memcmp(bytes, kKtx2Identifier.data(), kKtx2Identifier.size()) == 0;
}

bool loadImageDataPreservingKtx2(tinygltf::Image* image,
                                 const int        imageIndex,
                                 std::string*     err,
                                 std::string*     warn,
                                 int              reqWidth,
                                 int              reqHeight,
                                 const unsigned char* bytes,
                                 int              size,
                                 void*            userData)
{
    (void)imageIndex;

    const bool isKtx2 = image != nullptr
        && (image->mimeType == "image/ktx2"
            || hasExtension(std::filesystem::path(image->uri), ".ktx2")
            || hasKtx2Identifier(bytes, size));
    if(isKtx2)
    {
        image->image.assign(bytes, bytes + size);
        image->component = 0;
        image->bits = 8;
        image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;

        if(size >= 32)
        {
            uint32_t width = 0;
            uint32_t height = 0;
            std::memcpy(&width, bytes + 20, sizeof(uint32_t));
            std::memcpy(&height, bytes + 24, sizeof(uint32_t));
            image->width = static_cast<int>(width);
            image->height = static_cast<int>(height);
        }

        return true;
    }

    return tinygltf::LoadImageData(image, imageIndex, err, warn, reqWidth, reqHeight, bytes, size, userData);
}

bool readTinyGltfInt(const tinygltf::Value& value, int& out)
{
    if(value.IsNumber())
    {
        out = value.GetNumberAsInt();
        return true;
    }
    return false;
}

bool readExtensionTextureIndex(const tinygltf::Model& model, const tinygltf::Value& extension, const char* textureInfoName, int& out)
{
    if(!extension.IsObject())
    {
        return false;
    }

    const tinygltf::Value& textureInfo = extension.Get(textureInfoName);
    if(!textureInfo.IsObject())
    {
        return false;
    }

    int textureIndex = -1;
    if(!readTinyGltfInt(textureInfo.Get("index"), textureIndex))
    {
        return false;
    }

    const int imageIndex = resolveTextureSourceIndex(model, textureIndex);
    if(imageIndex < 0)
    {
        return false;
    }

    out = imageIndex;
    return true;
}

glm::vec4 readVec4ExtensionFactor(const tinygltf::Value& extension, const char* name, const glm::vec4& fallback)
{
    if(!extension.IsObject())
    {
        return fallback;
    }

    const tinygltf::Value& value = extension.Get(name);
    if(!value.IsArray() || value.ArrayLen() < 4)
    {
        return fallback;
    }

    glm::vec4 result = fallback;
    for(size_t i = 0; i < 4; ++i)
    {
        const tinygltf::Value& component = value.Get(i);
        if(component.IsNumber())
        {
            result[static_cast<glm::length_t>(i)] = static_cast<float>(component.GetNumberAsDouble());
        }
    }
    return result;
}

float readFloatExtensionFactor(const tinygltf::Value& extension, const char* name, float fallback)
{
    if(!extension.IsObject())
    {
        return fallback;
    }

    const tinygltf::Value& value = extension.Get(name);
    return value.IsNumber() ? static_cast<float>(value.GetNumberAsDouble()) : fallback;
}

glm::vec3 readVec3Value(const tinygltf::Value& object, const char* name, const glm::vec3& fallback)
{
    if(!object.IsObject())
    {
        return fallback;
    }

    const tinygltf::Value& value = object.Get(name);
    if(!value.IsArray() || value.ArrayLen() < 3)
    {
        return fallback;
    }

    glm::vec3 result = fallback;
    for(size_t i = 0; i < 3; ++i)
    {
        const tinygltf::Value& component = value.Get(i);
        if(component.IsNumber())
        {
            result[static_cast<glm::length_t>(i)] = static_cast<float>(component.GetNumberAsDouble());
        }
    }
    return result;
}

float readFloatValue(const tinygltf::Value& object, const char* name, float fallback)
{
    if(!object.IsObject())
    {
        return fallback;
    }

    const tinygltf::Value& value = object.Get(name);
    return value.IsNumber() ? static_cast<float>(value.GetNumberAsDouble()) : fallback;
}

std::string readStringValue(const tinygltf::Value& object, const char* name, const std::string& fallback = {})
{
    if(!object.IsObject())
    {
        return fallback;
    }

    const tinygltf::Value& value = object.Get(name);
    return value.IsString() ? value.Get<std::string>() : fallback;
}

}  // namespace

// Forward declarations for tangent generation
static std::vector<float> computeTangents(
    const std::vector<float>& positions,
    const std::vector<float>& normals,
    const std::vector<float>& texCoords,
    const std::vector<uint32_t>& indices
);
static void generateTangentsIfMissing(GltfMeshData& mesh);
static int resolveTextureSourceIndex(const tinygltf::Model& model, int textureIndex);
static void recordBasisuFallbackImages(const tinygltf::Model& model, GltfModel& outModel);
static bool readFloatAccessor(
    const tinygltf::Model& model,
    const tinygltf::Accessor& accessor,
    int expectedType,
    int componentCount,
    std::vector<float>& out
);

bool GltfLoader::load(const std::string& filepath, GltfModel& outModel) {
    outModel = {};
    m_nodeVisitState.clear();
    m_lightDefinitions.clear();
    outModel.sourcePath = filepath;
    outModel.sourceDirectory = std::filesystem::path(filepath).parent_path().string();

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    tinygltf::LoadImageDataOption imageLoadOptions;
    loader.SetImageLoader(loadImageDataPreservingKtx2, &imageLoadOptions);
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

    const std::filesystem::path sourceDirectory = std::filesystem::path(filepath).parent_path();
    for(const tinygltf::Buffer& buffer : model.buffers) {
        addExternalDependency(sourceDirectory, std::filesystem::path(buffer.uri), outModel);
    }
    for(const tinygltf::Image& image : model.images) {
        addExternalDependency(sourceDirectory, std::filesystem::path(image.uri), outModel);
        if(!image.uri.empty()) {
            std::filesystem::path ktx2Path(image.uri);
            ktx2Path.replace_extension(".ktx2");
            addExternalDependency(sourceDirectory, ktx2Path, outModel);
        }
    }

    if(!hasReasonableModelShape(model)) {
        m_lastError = "glTF contains unreasonable node, mesh, material, or image counts";
        return false;
    }

    m_nodeVisitState.assign(model.nodes.size(), 0u);

    // Process materials and images first (they are referenced by meshes)
    processMaterials(model, outModel);
    processImages(model, std::filesystem::path(filepath), outModel);
    recordBasisuFallbackImages(model, outModel);
    processLightDefinitions(model);

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
            if (!processNode(model, nodeIndex, -1, glm::mat4(1.0f), outModel)) {
                return false;
            }
        }
    } else {
        // No scenes - try processing all nodes directly
        for (int i = 0; i < static_cast<int>(model.nodes.size()); ++i) {
            if (!processNode(model, i, -1, glm::mat4(1.0f), outModel)) {
                return false;
            }
        }
    }

    const uint64_t estimatedPayloadBytes = estimateModelPayloadBytes(outModel);
    if(estimatedPayloadBytes > kMaxReasonableModelPayloadBytes) {
        m_lastError = "glTF payload exceeds safe memory budget (estimated "
                    + std::to_string(estimatedPayloadBytes)
                    + " bytes, limit "
                    + std::to_string(kMaxReasonableModelPayloadBytes)
                    + " bytes)";
        return false;
    }

    return true;
}

void GltfLoader::processLightDefinitions(const tinygltf::Model& model)
{
    m_lightDefinitions.clear();

    if(!model.lights.empty())
    {
        m_lightDefinitions.reserve(model.lights.size());
        for(size_t lightIndex = 0; lightIndex < model.lights.size(); ++lightIndex)
        {
            const tinygltf::Light& gltfLight = model.lights[lightIndex];
            SceneLight light{};
            light.name = gltfLight.name.empty() ? ("Light " + std::to_string(lightIndex)) : gltfLight.name;
            if(gltfLight.type == "directional")
            {
                light.type = SceneLightType::directional;
            }
            else if(gltfLight.type == "spot")
            {
                light.type = SceneLightType::spot;
            }
            else
            {
                light.type = SceneLightType::point;
            }
            if(gltfLight.color.size() >= 3)
            {
                light.color = glm::vec3(static_cast<float>(gltfLight.color[0]),
                                        static_cast<float>(gltfLight.color[1]),
                                        static_cast<float>(gltfLight.color[2]));
            }
            light.intensity = static_cast<float>(gltfLight.intensity);
            light.range = static_cast<float>(gltfLight.range);
            light.innerConeAngle = static_cast<float>(gltfLight.spot.innerConeAngle);
            light.outerConeAngle = static_cast<float>(gltfLight.spot.outerConeAngle);
            light.outerConeAngle = std::max(light.outerConeAngle, light.innerConeAngle + 0.001f);
            m_lightDefinitions.push_back(std::move(light));
        }
        return;
    }

    const auto extensionIt = model.extensions.find("KHR_lights_punctual");
    if(extensionIt == model.extensions.end() || !extensionIt->second.IsObject())
    {
        return;
    }

    const tinygltf::Value& lights = extensionIt->second.Get("lights");
    if(!lights.IsArray())
    {
        return;
    }

    m_lightDefinitions.reserve(lights.ArrayLen());
    for(size_t lightIndex = 0; lightIndex < lights.ArrayLen(); ++lightIndex)
    {
        const tinygltf::Value& lightValue = lights.Get(static_cast<int>(lightIndex));
        if(!lightValue.IsObject())
        {
            m_lightDefinitions.push_back(SceneLight{});
            continue;
        }

        SceneLight light{};
        light.name = readStringValue(lightValue, "name", "Light " + std::to_string(lightIndex));
        const std::string type = readStringValue(lightValue, "type", "point");
        if(type == "directional")
        {
            light.type = SceneLightType::directional;
        }
        else if(type == "spot")
        {
            light.type = SceneLightType::spot;
        }
        else
        {
            light.type = SceneLightType::point;
        }

        light.color = readVec3Value(lightValue, "color", glm::vec3(1.0f));
        light.intensity = readFloatValue(lightValue, "intensity", 1.0f);
        light.range = readFloatValue(lightValue, "range", 0.0f);
        light.innerConeAngle = 0.0f;
        light.outerConeAngle = glm::quarter_pi<float>();

        const tinygltf::Value& spot = lightValue.Get("spot");
        if(spot.IsObject())
        {
            light.innerConeAngle = readFloatValue(spot, "innerConeAngle", light.innerConeAngle);
            light.outerConeAngle = readFloatValue(spot, "outerConeAngle", light.outerConeAngle);
        }
        light.outerConeAngle = std::max(light.outerConeAngle, light.innerConeAngle + 0.001f);

        m_lightDefinitions.push_back(std::move(light));
    }
}

bool GltfLoader::processNode(const tinygltf::Model& model,
                             int nodeIndex,
                             int parentNodeIndex,
                             const glm::mat4& parentTransform,
                             GltfModel& outModel) {
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size())) {
        m_lastError = "Invalid node index: " + std::to_string(nodeIndex);
        return false;
    }

    if(static_cast<size_t>(nodeIndex) >= m_nodeVisitState.size()) {
        m_lastError = "Node visit state out of range";
        return false;
    }

    if(m_nodeVisitState[static_cast<size_t>(nodeIndex)] == 1u) {
        m_lastError = "glTF node graph contains a cycle";
        return false;
    }

    if(m_nodeVisitState[static_cast<size_t>(nodeIndex)] == 2u) {
        return true;
    }

    m_nodeVisitState[static_cast<size_t>(nodeIndex)] = 1u;

    const auto& node = model.nodes[nodeIndex];

    // Compute local transform
    glm::mat4 localTransform(1.0f);
    glm::vec3 translation(0.0f);
    glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 scale(1.0f);

    if (node.matrix.size() == 16) {
        // Use matrix if provided
        localTransform = glm::make_mat4(node.matrix.data());
        glm::vec3 skew(0.0f);
        glm::vec4 perspective(0.0f);
        if(!glm::decompose(localTransform, scale, rotation, translation, skew, perspective)) {
            translation = glm::vec3(localTransform[3]);
            rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            scale = glm::vec3(1.0f);
        }
    } else {
        // Build from TRS components
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
        localTransform = composeTransform(translation, rotation, scale);

        // Debug: verify translation is preserved in final matrix
        static bool printedTRS = false;
        if(!printedTRS) {
            printf("GltfLoader: First node TRS debug:\n");
            printf("  translation: (%f, %f, %f)\n", translation.x, translation.y, translation.z);
            printf("  scale: (%f, %f, %f)\n", scale.x, scale.y, scale.z);
            printf("  Result matrix column 3 (should be translation):\n");
            printf("    [%f, %f, %f, %f]\n",
                   localTransform[3][0], localTransform[3][1], localTransform[3][2], localTransform[3][3]);
            printedTRS = true;
        }
    }

    if(glm::length(rotation) < 0.0001f) {
        rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    } else {
        rotation = glm::normalize(rotation);
    }

    GltfNodeData nodeData;
    nodeData.name = node.name.empty() ? ("Node " + std::to_string(nodeIndex)) : node.name;
    nodeData.parent = parentNodeIndex;
    nodeData.translation = translation;
    nodeData.rotation = rotation;
    nodeData.rotationEulerDegrees = sanitizeEulerDegrees(glm::eulerAngles(rotation));
    nodeData.scale = scale;
    nodeData.localTransform = composeTransform(nodeData.translation, nodeData.rotation, nodeData.scale);
    nodeData.worldTransform = parentTransform * nodeData.localTransform;
    nodeData.firstMeshIndex = static_cast<uint32_t>(outModel.meshes.size());

    const int currentNodeIndex = static_cast<int>(outModel.nodes.size());
    outModel.nodes.push_back(nodeData);
    if(parentNodeIndex >= 0) {
        outModel.nodes[parentNodeIndex].children.push_back(currentNodeIndex);
    } else {
        outModel.rootNodes.push_back(currentNodeIndex);
    }

    int lightIndex = node.light;
    const auto lightExtensionIt = node.extensions.find("KHR_lights_punctual");
    if(lightIndex < 0 && lightExtensionIt != node.extensions.end() && lightExtensionIt->second.IsObject())
    {
        readTinyGltfInt(lightExtensionIt->second.Get("light"), lightIndex);
    }
    if(lightIndex >= 0 && static_cast<size_t>(lightIndex) < m_lightDefinitions.size())
    {
        SceneLight light = m_lightDefinitions[static_cast<size_t>(lightIndex)];
        light.nodeIndex = currentNodeIndex;
        if(light.name.empty())
        {
            light.name = nodeData.name;
        }
        outModel.lights.push_back(std::move(light));
    }

    // Process mesh if present
    if (node.mesh >= 0) {
        if (!processMesh(model, node.mesh, outModel.nodes[currentNodeIndex].worldTransform, outModel)) {
            return false;
        }
    }
    outModel.nodes[currentNodeIndex].meshCount =
        static_cast<uint32_t>(outModel.meshes.size()) - outModel.nodes[currentNodeIndex].firstMeshIndex;

    // Recursively process children
    for (int childIndex : node.children) {
        if (!processNode(model, childIndex, currentNodeIndex, outModel.nodes[currentNodeIndex].worldTransform, outModel)) {
            return false;
        }
    }

    m_nodeVisitState[static_cast<size_t>(nodeIndex)] = 2u;

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
        size_t vertexCount = posAccessor.count;
        if(vertexCount > kMaxReasonableAccessorElements) {
            m_lastError = "Mesh POSITION accessor has unreasonable vertex count";
            return false;
        }
        if (!readFloatAccessor(model, posAccessor, TINYGLTF_TYPE_VEC3, 3, meshData.positions)) {
            m_lastError = "Mesh POSITION accessor must be VEC3 float";
            return false;
        }

        // Get normals (optional)
        auto normIt = primitive.attributes.find("NORMAL");
        if (normIt != primitive.attributes.end()) {
            const auto& normAccessor = model.accessors[normIt->second];
            if (!readFloatAccessor(model, normAccessor, TINYGLTF_TYPE_VEC3, 3, meshData.normals)) {
                m_lastError = "Mesh NORMAL accessor must be VEC3 float";
                return false;
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
            if (!readFloatAccessor(model, texAccessor, TINYGLTF_TYPE_VEC2, 2, meshData.texCoords)) {
                m_lastError = "Mesh TEXCOORD_0 accessor must be VEC2 float";
                return false;
            }
        } else {
            // Generate default UVs
            meshData.texCoords.resize(vertexCount * 2, 0.0f);
        }

        // Get tangents (optional, but preferred over runtime generation for normal maps)
        auto tangentIt = primitive.attributes.find("TANGENT");
        if (tangentIt != primitive.attributes.end()) {
            const auto& tangentAccessor = model.accessors[tangentIt->second];
            if (!readFloatAccessor(model, tangentAccessor, TINYGLTF_TYPE_VEC4, 4, meshData.tangents)) {
                m_lastError = "Mesh TANGENT accessor must be VEC4 float";
                return false;
            }
        }

        // Get indices
        if (primitive.indices >= 0) {
            const auto& indexAccessor = model.accessors[primitive.indices];
            if(indexAccessor.count > kMaxReasonableAccessorElements) {
                m_lastError = "Mesh index accessor has unreasonable index count";
                return false;
            }
            const auto& indexBufferView = model.bufferViews[indexAccessor.bufferView];
            const auto& indexBuffer = model.buffers[indexBufferView.buffer];

            meshData.indices.reserve(indexAccessor.count);

            // Handle different index component types
            const size_t componentSize = indexAccessor.ByteStride(indexBufferView) != 0
                ? indexAccessor.ByteStride(indexBufferView)
                : tinygltf::GetComponentSizeInBytes(indexAccessor.componentType);
            const size_t byteOffset = indexBufferView.byteOffset + indexAccessor.byteOffset;
            const size_t byteSize = static_cast<size_t>(indexAccessor.count) * componentSize;
            if(byteOffset > indexBuffer.data.size() || byteSize > indexBuffer.data.size() - byteOffset) {
                m_lastError = "Mesh index accessor exceeds source buffer bounds";
                return false;
            }
            const uint8_t* indexData = indexBuffer.data.data() + byteOffset;

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
            data.baseColorTexture = resolveTextureSourceIndex(model, pbr.baseColorTexture.index);
        }

        // Metallic-Roughness
        data.metallicFactor = static_cast<float>(pbr.metallicFactor);
        data.roughnessFactor = static_cast<float>(pbr.roughnessFactor);
        if (pbr.metallicRoughnessTexture.index >= 0) {
            data.metallicRoughnessTexture = resolveTextureSourceIndex(model, pbr.metallicRoughnessTexture.index);
        }

        // Normal map
        if (mat.normalTexture.index >= 0) {
            data.normalTexture = resolveTextureSourceIndex(model, mat.normalTexture.index);
            data.normalScale = static_cast<float>(mat.normalTexture.scale);
        }

        // Occlusion
        if (mat.occlusionTexture.index >= 0) {
            data.occlusionTexture = resolveTextureSourceIndex(model, mat.occlusionTexture.index);
            data.occlusionStrength = static_cast<float>(mat.occlusionTexture.strength);
        }

        // Emissive
        data.emissiveFactor = glm::vec3(
            static_cast<float>(mat.emissiveFactor[0]),
            static_cast<float>(mat.emissiveFactor[1]),
            static_cast<float>(mat.emissiveFactor[2])
        );
        if (mat.emissiveTexture.index >= 0) {
            data.emissiveTexture = resolveTextureSourceIndex(model, mat.emissiveTexture.index);
        }

        const auto specGlossIt = mat.extensions.find("KHR_materials_pbrSpecularGlossiness");
        if(specGlossIt != mat.extensions.end() && specGlossIt->second.IsObject())
        {
            const tinygltf::Value& specGloss = specGlossIt->second;
            int diffuseTexture = -1;
            if(readExtensionTextureIndex(model, specGloss, "diffuseTexture", diffuseTexture))
            {
                data.baseColorTexture = diffuseTexture;
            }
            data.baseColorFactor = readVec4ExtensionFactor(specGloss, "diffuseFactor", data.baseColorFactor);

            int specularGlossinessTexture = -1;
            if(readExtensionTextureIndex(model, specGloss, "specularGlossinessTexture", specularGlossinessTexture))
            {
                data.metallicRoughnessTexture = specularGlossinessTexture;
            }
            data.materialWorkflow = 1;
            data.metallicFactor = 0.0f;
            data.roughnessFactor = readFloatExtensionFactor(specGloss, "glossinessFactor", 1.0f);
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

void GltfLoader::processImages(const tinygltf::Model& model, const std::filesystem::path& sourcePath, GltfModel& outModel) {
    (void)sourcePath;
    outModel.images.clear();
    outModel.images.resize(model.images.size());

    for (size_t imageIndex = 0; imageIndex < model.images.size(); ++imageIndex) {
        const auto& image = model.images[imageIndex];
        const bool isKtx2 = image.mimeType == "image/ktx2"
                         || hasExtension(std::filesystem::path(image.uri), ".ktx2")
                         || hasKtx2Identifier(image.image.data(), static_cast<int>(image.image.size()));

        GltfImageData imageData;
        imageData.width = image.width;
        imageData.height = image.height;
        imageData.channels = 4;
        imageData.uri = image.uri;
        imageData.mimeType = image.mimeType;
        imageData.isKtx2 = isKtx2;

        if(imageData.uri.empty() && !image.name.empty())
        {
            imageData.uri = image.name;
        }

        if(image.width < 0 || image.height < 0 || image.component < 0) {
            outModel.images[imageIndex] = std::move(imageData);
            continue;
        }
        if(static_cast<size_t>(image.width) > kMaxReasonableImageDimension
           || static_cast<size_t>(image.height) > kMaxReasonableImageDimension
           || (!isKtx2 && image.component > 4)) {
            outModel.images[imageIndex] = std::move(imageData);
            continue;
        }
        if(image.image.size() > kMaxReasonableImageBytes) {
            outModel.images[imageIndex] = std::move(imageData);
            continue;
        }

        if(isKtx2)
        {
            imageData.ktx2Data = image.image;
        }
        else
        {
            // Normalize to RGBA8 so the upload path can generate mipmaps reliably.
            imageData.pixels = expandToRgba8(image.image, image.width, image.height, image.component);
        }

        outModel.images[imageIndex] = std::move(imageData);
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

static int resolveTextureSourceIndex(const tinygltf::Model& model, int textureIndex) {
    if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size())) {
        return -1;
    }

    const tinygltf::Texture& texture = model.textures[textureIndex];
    int imageIndex = texture.source;
    const auto basisuIt = texture.extensions.find("KHR_texture_basisu");
    if(basisuIt != texture.extensions.end() && basisuIt->second.IsObject())
    {
        const tinygltf::Value& sourceValue = basisuIt->second.Get("source");
        readTinyGltfInt(sourceValue, imageIndex);
    }

    if (imageIndex < 0 || imageIndex >= static_cast<int>(model.images.size())) {
        return -1;
    }

    return imageIndex;
}

static void recordBasisuFallbackImages(const tinygltf::Model& model, GltfModel& outModel) {
    for(const tinygltf::Texture& texture : model.textures)
    {
        const auto basisuIt = texture.extensions.find("KHR_texture_basisu");
        if(basisuIt == texture.extensions.end() || !basisuIt->second.IsObject())
        {
            continue;
        }

        const tinygltf::Value& sourceValue = basisuIt->second.Get("source");
        int basisImage = -1;
        if(!readTinyGltfInt(sourceValue, basisImage))
        {
            continue;
        }
        const int fallbackImage = texture.source;
        if(basisImage < 0 || fallbackImage < 0
           || basisImage >= static_cast<int>(outModel.images.size())
           || fallbackImage >= static_cast<int>(outModel.images.size()))
        {
            continue;
        }

        outModel.images[static_cast<size_t>(basisImage)].fallbackImage = fallbackImage;
    }
}

static bool readFloatAccessor(
    const tinygltf::Model& model,
    const tinygltf::Accessor& accessor,
    int expectedType,
    int componentCount,
    std::vector<float>& out
) {
    if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size())) {
        return false;
    }
    if (accessor.type != expectedType || accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
        return false;
    }

    const auto& bufferView = model.bufferViews[accessor.bufferView];
    if (bufferView.buffer < 0 || bufferView.buffer >= static_cast<int>(model.buffers.size())) {
        return false;
    }
    if(accessor.count > kMaxReasonableAccessorElements) {
        return false;
    }

    const auto& buffer = model.buffers[bufferView.buffer];
    const size_t stride = accessor.ByteStride(bufferView);
    const size_t packedSize = sizeof(float) * static_cast<size_t>(componentCount);
    const size_t byteStride = stride == 0 ? packedSize : stride;
    const size_t byteOffset = bufferView.byteOffset + accessor.byteOffset;
    const size_t byteSize = accessor.count == 0 ? 0 : ((accessor.count - 1) * byteStride + packedSize);
    if(byteOffset > buffer.data.size() || byteSize > buffer.data.size() - byteOffset) {
        return false;
    }
    const uint8_t* base = buffer.data.data() + byteOffset;

    out.resize(accessor.count * static_cast<size_t>(componentCount));
    for (size_t i = 0; i < accessor.count; ++i) {
        std::memcpy(out.data() + i * componentCount, base + i * byteStride, packedSize);
    }

    return true;
}

}  // namespace demo
