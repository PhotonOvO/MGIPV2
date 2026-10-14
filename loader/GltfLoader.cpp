#include "GltfLoader.h"
#include "../common/ProfilerMarkers.h"

#include <mgif/scene_editor/GltfDocument.h>
#include <mgif/scene_editor/GltfDocumentIO.h>
#include <mgif/scene_editor/GltfSemanticCompare.h>

#include <mgif/scene_editor/EditorLimits.h>

#include <tiny_gltf.h>
#include <stb_image.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <utility>

namespace demo {

static int resolveTextureSourceIndex(const tinygltf::Model& model, int textureIndex);

namespace {

constexpr size_t kMaxReasonableNodes = editor::kDefaultEditorLimits.maxNodes;
constexpr size_t kMaxReasonableMeshes = editor::kDefaultEditorLimits.maxMeshes;
constexpr size_t kMaxReasonableMaterials = editor::kDefaultEditorLimits.maxMaterials;
constexpr size_t kMaxReasonableImages = editor::kDefaultEditorLimits.maxImages;
constexpr size_t kMaxReasonableCameras = editor::kDefaultEditorLimits.maxCameras;
constexpr size_t kMaxReasonableLights = editor::kDefaultEditorLimits.maxLights;
constexpr size_t kMaxReasonableImageDimension = editor::kDefaultEditorLimits.maxImageDimension;
constexpr size_t kMaxReasonableImageBytes = 1u << 28;
constexpr size_t kMaxReasonableAccessorElements = editor::kDefaultEditorLimits.maxAccessorElements;
// Bistro-scale scenes legitimately exceed a 512 MiB decoded payload once
// textures are expanded to RGBA8, so keep a higher but still explicit cap.
constexpr uint64_t kMaxReasonableModelPayloadBytes = 4ull << 30;
constexpr std::array<uint8_t, 12> kKtx2Identifier{
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

constexpr float kGltfDirectionalLuxPerBlenderStrength = 683.0f;

float convertGltfLightIntensity(SceneLightType type, float intensity)
{
    // Blender's standard glTF export converts Sun strength to lux using 683 lm/W.
    return type == SceneLightType::directional
        ? intensity / kGltfDirectionalLuxPerBlenderStrength
        : intensity;
}

void assignImageColorSpaces(GltfModel& model)
{
    constexpr uint8_t kColorUsage = 1u << 0u;
    constexpr uint8_t kLinearUsage = 1u << 1u;
    std::vector<uint8_t> imageUsage(model.images.size(), 0u);
    for(size_t textureIndex = 0; textureIndex < model.textures.size(); ++textureIndex)
    {
        GltfTextureData& texture = model.textures[textureIndex];
        if(texture.resolvedImageIndex >= 0
           && static_cast<size_t>(texture.resolvedImageIndex) < imageUsage.size())
        {
            imageUsage[static_cast<size_t>(texture.resolvedImageIndex)] |= texture.usage;
        }
    }

    const size_t originalImageCount = model.images.size();
    std::vector<int> colorDuplicates(originalImageCount, -1);
    for(size_t imageIndex = 0; imageIndex < originalImageCount; ++imageIndex)
    {
        const bool color = (imageUsage[imageIndex] & kColorUsage) != 0u;
        const bool linear = (imageUsage[imageIndex] & kLinearUsage) != 0u;
        if(color && !linear)
        {
            model.images[imageIndex].isSrgb = true;
        }
        else if(color && linear)
        {
            GltfImageData colorImage = model.images[imageIndex];
            colorImage.isSrgb = true;
            colorDuplicates[imageIndex] = static_cast<int>(model.images.size());
            model.images.push_back(std::move(colorImage));
            model.images[imageIndex].isSrgb = false;
        }
    }

    for(GltfTextureData& texture : model.textures)
    {
        const int imageIndex = texture.resolvedImageIndex;
        if(imageIndex < 0 || static_cast<size_t>(imageIndex) >= originalImageCount)
        {
            continue;
        }
        const bool color = (texture.usage & kColorUsage) != 0u;
        const bool linear = (texture.usage & kLinearUsage) != 0u;
        texture.colorImageIndex = color
            ? (colorDuplicates[static_cast<size_t>(imageIndex)] >= 0
                ? colorDuplicates[static_cast<size_t>(imageIndex)]
                : imageIndex)
            : -1;
        texture.linearImageIndex = linear || !color ? imageIndex : -1;
        texture.canonicalImageIndex = texture.linearImageIndex >= 0
            ? texture.linearImageIndex
            : texture.colorImageIndex;
    }

    for(GltfMaterialData& material : model.materials)
    {
        const auto selectColorDuplicate = [&](int& imageIndex)
        {
            if(imageIndex >= 0
               && static_cast<size_t>(imageIndex) < colorDuplicates.size()
               && colorDuplicates[static_cast<size_t>(imageIndex)] >= 0)
            {
                imageIndex = colorDuplicates[static_cast<size_t>(imageIndex)];
            }
        };
        selectColorDuplicate(material.baseColorTexture);
        selectColorDuplicate(material.emissiveTexture);
    }
}

bool hasReasonableModelShape(const tinygltf::Model& model)
{
    return model.nodes.size() <= kMaxReasonableNodes
        && model.meshes.size() <= kMaxReasonableMeshes
        && model.materials.size() <= kMaxReasonableMaterials
        && model.images.size() <= kMaxReasonableImages
        && model.cameras.size() <= kMaxReasonableCameras
        && model.lights.size() <= kMaxReasonableLights;
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

bool checkedImageByteCount(int width, int height, size_t channels, size_t& outBytes)
{
    outBytes = 0;
    if(width <= 0 || height <= 0 || channels == 0)
    {
        return false;
    }

    const uint64_t widthValue = static_cast<uint64_t>(width);
    const uint64_t heightValue = static_cast<uint64_t>(height);
    if(widthValue > std::numeric_limits<uint64_t>::max() / heightValue)
    {
        return false;
    }
    const uint64_t pixelCount = widthValue * heightValue;
    if(pixelCount > std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(channels))
    {
        return false;
    }
    const uint64_t byteCount = pixelCount * static_cast<uint64_t>(channels);
    if(byteCount > static_cast<uint64_t>(kMaxReasonableImageBytes)
       || byteCount > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
    {
        return false;
    }
    outBytes = static_cast<size_t>(byteCount);
    return true;
}

bool expandToRgba8(const std::vector<uint8_t>& pixels,
                   int width,
                   int height,
                   int channels,
                   std::vector<uint8_t>& outRgba)
{
    outRgba.clear();
    if(channels < 1 || channels > 4)
    {
        return false;
    }

    size_t sourceBytes = 0;
    size_t rgbaBytes = 0;
    if(!checkedImageByteCount(width, height, static_cast<size_t>(channels), sourceBytes)
       || !checkedImageByteCount(width, height, 4u, rgbaBytes)
       || pixels.size() != sourceBytes)
    {
        return false;
    }

    try
    {
        if(channels == 4)
        {
            outRgba = pixels;
            return true;
        }

        outRgba.assign(rgbaBytes, 255u);
    }
    catch(...)
    {
        outRgba.clear();
        return false;
    }

    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    for(size_t i = 0; i < pixelCount; ++i)
    {
        const size_t srcIndex = i * static_cast<size_t>(channels);
        const size_t dstIndex = i * 4u;

        switch(channels)
        {
            case 1:
                outRgba[dstIndex + 0] = pixels[srcIndex + 0];
                outRgba[dstIndex + 1] = pixels[srcIndex + 0];
                outRgba[dstIndex + 2] = pixels[srcIndex + 0];
                break;
            case 2:
                outRgba[dstIndex + 0] = pixels[srcIndex + 0];
                outRgba[dstIndex + 1] = pixels[srcIndex + 0];
                outRgba[dstIndex + 2] = pixels[srcIndex + 0];
                outRgba[dstIndex + 3] = pixels[srcIndex + 1];
                break;
            case 3:
                outRgba[dstIndex + 0] = pixels[srcIndex + 0];
                outRgba[dstIndex + 1] = pixels[srcIndex + 1];
                outRgba[dstIndex + 2] = pixels[srcIndex + 2];
                break;
            default: break;
        }
    }

    return true;
}

bool hasPngIdentifier(const unsigned char* bytes, size_t size)
{
    static constexpr std::array<uint8_t, 8> identifier{
        0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    return bytes != nullptr && size >= identifier.size()
        && std::memcmp(bytes, identifier.data(), identifier.size()) == 0;
}

bool hasJpegIdentifier(const unsigned char* bytes, size_t size)
{
    return bytes != nullptr && size >= 3u
        && bytes[0] == 0xFFu && bytes[1] == 0xD8u && bytes[2] == 0xFFu;
}

enum class EncodedImageKind : uint8_t
{
    unspecified,
    png,
    jpeg,
    unsupported,
};

EncodedImageKind declaredEncodedImageKind(const tinygltf::Image& image)
{
    std::string mime = image.mimeType;
    std::transform(mime.begin(), mime.end(), mime.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if(mime == "image/png")
    {
        return EncodedImageKind::png;
    }
    if(mime == "image/jpeg" || mime == "image/jpg")
    {
        return EncodedImageKind::jpeg;
    }
    if(!mime.empty())
    {
        return EncodedImageKind::unsupported;
    }

    std::string extension = std::filesystem::path(image.uri).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if(extension == ".png")
    {
        return EncodedImageKind::png;
    }
    if(extension == ".jpg" || extension == ".jpeg")
    {
        return EncodedImageKind::jpeg;
    }
    if(extension == ".bmp" || extension == ".gif" || extension == ".webp")
    {
        return EncodedImageKind::unsupported;
    }
    return EncodedImageKind::unspecified;
}

bool decodeEncodedImageRgba8(const tinygltf::Image& image,
                             GltfImageData& outImage,
                             std::string& error)
{
    const size_t encodedSize = image.image.size();
    if(encodedSize == 0 || encodedSize > kMaxReasonableImageBytes
       || encodedSize > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        error = "encoded payload exceeds the bounded input size";
        return false;
    }

    const bool png = hasPngIdentifier(image.image.data(), encodedSize);
    const bool jpeg = hasJpegIdentifier(image.image.data(), encodedSize);
    const EncodedImageKind declaredKind = declaredEncodedImageKind(image);
    if(declaredKind == EncodedImageKind::unsupported)
    {
        error = "declared encoded format is not PNG or JPEG";
        return false;
    }
    if((declaredKind == EncodedImageKind::png && !png)
       || (declaredKind == EncodedImageKind::jpeg && !jpeg))
    {
        error = "payload identifier does not match the declared encoded format";
        return false;
    }
    if(!png && !jpeg)
    {
        error = "payload does not have a PNG or JPEG identifier";
        return false;
    }

    int decodedWidth = 0;
    int decodedHeight = 0;
    int sourceChannels = 0;
    if(!stbi_info_from_memory(image.image.data(), static_cast<int>(encodedSize),
                              &decodedWidth, &decodedHeight, &sourceChannels))
    {
        error = "PNG/JPEG header could not be decoded";
        return false;
    }
    if(sourceChannels < 1 || sourceChannels > 4
       || (image.component > 0 && image.component != sourceChannels))
    {
        error = "encoded component metadata is inconsistent";
        return false;
    }

    size_t rgbaBytes = 0;
    if(static_cast<size_t>(decodedWidth) > kMaxReasonableImageDimension
       || static_cast<size_t>(decodedHeight) > kMaxReasonableImageDimension
       || !checkedImageByteCount(decodedWidth, decodedHeight, 4u, rgbaBytes))
    {
        error = "decoded dimensions exceed the bounded RGBA8 payload";
        return false;
    }
    if((image.width > 0 && image.width != decodedWidth)
       || (image.height > 0 && image.height != decodedHeight))
    {
        error = "encoded dimensions disagree with the TinyGLTF authority";
        return false;
    }
    std::vector<uint8_t> decodedPixels;
    try
    {
        decodedPixels.resize(rgbaBytes);
    }
    catch(...)
    {
        error = "decoded RGBA8 allocation failed";
        return false;
    }

    int loadedWidth = 0;
    int loadedHeight = 0;
    int loadedChannels = 0;
    stbi_uc* decoded = stbi_load_from_memory(
        image.image.data(), static_cast<int>(encodedSize),
        &loadedWidth, &loadedHeight, &loadedChannels, 4);
    if(decoded == nullptr)
    {
        error = "PNG/JPEG pixels could not be decoded";
        return false;
    }

    const bool dimensionsMatch = loadedWidth == decodedWidth
        && loadedHeight == decodedHeight;
    size_t loadedBytes = 0;
    const bool loadedSizeValid = checkedImageByteCount(
        loadedWidth, loadedHeight, 4u, loadedBytes) && loadedBytes == rgbaBytes;
    if(!dimensionsMatch || !loadedSizeValid)
    {
        stbi_image_free(decoded);
        error = "decoded RGBA8 dimensions are inconsistent";
        return false;
    }

    std::copy(decoded, decoded + rgbaBytes, decodedPixels.begin());
    stbi_image_free(decoded);
    (void)loadedChannels;

    outImage.width = loadedWidth;
    outImage.height = loadedHeight;
    outImage.channels = 4;
    outImage.pixels = std::move(decodedPixels);
    return true;
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

bool hasKtx2Identifier(const unsigned char* bytes, size_t size)
{
    return bytes != nullptr
        && size >= kKtx2Identifier.size()
        && std::memcmp(bytes, kKtx2Identifier.data(), kKtx2Identifier.size()) == 0;
}

// 自定义 tinygltf 文件读取：png/jpg 引用存在同名 .ktx2 时优先读取 ktx2 字节。
// 下游 loadImageDataPreservingKtx2 / processImages 均按 KTX2 魔数识别（与 URI 无关），
// 离线 texture_converter 产出的 BC7+完整 mip 链纹理因此无需改写 glTF 即可生效。
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
    GltfModel candidate;
    if(!loadCandidate(filepath, candidate)) {
        return false;
    }
    outModel = std::move(candidate);
    return true;
}

bool GltfLoader::loadCandidate(const std::string& filepath, GltfModel& outModel) {
    const auto result = editor::GltfDocumentIO::loadCandidate(std::filesystem::path(filepath));
    if(!result.succeeded() || result.candidate() == nullptr)
    {
        m_lastError = result.message().empty() ? "Failed to load glTF document" : result.message();
        return false;
    }
    return decode(*result.candidate(), outModel);
}

bool GltfLoader::decode(const editor::GltfDocument& document, GltfModel& outModel)
{
    return decodeModel(document.authorityModel(), document.sourcePath(), &document, outModel);
}

bool GltfLoader::decodeModel(const tinygltf::Model& model,
                             const std::filesystem::path& sourcePath,
                             const editor::GltfDocument* document,
                             GltfModel& outModel)
{
    VKDEMO_CPU_SCOPE("Scene.Gltf.DecodeDocument");
    m_lastError.clear();
    outModel = {};
    m_nodeVisitState.clear();
    m_lightDefinitions.clear();
    outModel.sourcePath = sourcePath.string();
    outModel.sourceDirectory = sourcePath.parent_path().string();

    if(document != nullptr)
    {
        outModel.dependencies.reserve(document->dependencies().size());
        for(const editor::GltfDependency& dependency : document->dependencies())
        {
            outModel.dependencies.push_back(GltfDependencyData{
                .relativePath = dependency.resolvedPath.empty()
                    ? dependency.uri
                    : dependency.resolvedPath.lexically_relative(sourcePath.parent_path()).generic_string(),
                .fileSize = dependency.byteSize,
            });
        }
    }
    if(!hasReasonableModelShape(model))
    {
        m_lastError = "glTF contains unreasonable node, mesh, material, or image counts";
        return false;
    }

    outModel.textures.reserve(model.textures.size());
    for(size_t textureIndex = 0; textureIndex < model.textures.size(); ++textureIndex)
    {
        const tinygltf::Texture& texture = model.textures[textureIndex];
        outModel.textures.push_back(GltfTextureData{
            .sourceImageIndex = texture.source,
            .resolvedImageIndex = resolveTextureSourceIndex(
                model, static_cast<int>(textureIndex)),
            .samplerIndex = texture.sampler,
            .name = texture.name,
            .extensionsJson = texture.extensions_json_string,
            .extrasJson = texture.extras_json_string,
        });
    }
    outModel.samplers.reserve(model.samplers.size());
    for(const tinygltf::Sampler& sampler : model.samplers)
    {
        outModel.samplers.push_back(GltfSamplerData{
            .magFilter = sampler.magFilter,
            .minFilter = sampler.minFilter,
            .wrapS = sampler.wrapS,
            .wrapT = sampler.wrapT,
            .name = sampler.name,
            .extensionsJson = sampler.extensions_json_string,
            .extrasJson = sampler.extras_json_string,
        });
    }

    processMaterials(model, outModel);
    processImages(model, sourcePath, outModel);
    if(!m_lastError.empty())
    {
        outModel = {};
        return false;
    }
    recordBasisuFallbackImages(model, outModel);
    assignImageColorSpaces(outModel);
    processLightDefinitions(model);
    processAnimations(model, outModel);

    outModel.name = sourcePath.filename().string();
    m_nodeVisitState.assign(model.nodes.size(), 0u);
    int sceneIndex = model.defaultScene;
    if(document != nullptr)
    {
        const auto currentScene = document->currentSceneIndex();
        if(currentScene)
        {
            sceneIndex = static_cast<int>(*currentScene);
        }
    }
    if(sceneIndex < 0 && !model.scenes.empty())
    {
        sceneIndex = 0;
    }

    if(sceneIndex >= 0 && sceneIndex < static_cast<int>(model.scenes.size()))
    {
        for(const int nodeIndex : model.scenes[static_cast<std::size_t>(sceneIndex)].nodes)
        {
            if(!processNode(model, nodeIndex, -1, glm::mat4(1.0f), true, outModel))
            {
                return false;
            }
        }
    }
    else
    {
        for(int nodeIndex = 0; nodeIndex < static_cast<int>(model.nodes.size()); ++nodeIndex)
        {
            if(!processNode(model, nodeIndex, -1, glm::mat4(1.0f), true, outModel))
            {
                return false;
            }
        }
    }

    const uint64_t estimatedPayloadBytes = estimateModelPayloadBytes(outModel);
    if(estimatedPayloadBytes > kMaxReasonableModelPayloadBytes)
    {
        m_lastError = "glTF payload exceeds safe memory budget (estimated "
                    + std::to_string(estimatedPayloadBytes)
                    + " bytes, limit "
                    + std::to_string(kMaxReasonableModelPayloadBytes)
                    + " bytes)";
        return false;
    }
    return true;
}

void GltfLoader::processAnimations(const tinygltf::Model& model, GltfModel& outModel)
{
    outModel.animations.clear();
    outModel.animations.reserve(model.animations.size());
    for(const tinygltf::Animation& animation : model.animations)
    {
        GltfAnimationData projected;
        projected.name = animation.name;
        projected.samplers.reserve(animation.samplers.size());
        for(const tinygltf::AnimationSampler& sampler : animation.samplers)
        {
            projected.samplers.push_back(GltfAnimationSamplerData{
                .inputAccessor = sampler.input,
                .outputAccessor = sampler.output,
                .interpolation = sampler.interpolation,
            });
        }
        projected.channels.reserve(animation.channels.size());
        for(const tinygltf::AnimationChannel& channel : animation.channels)
        {
            projected.channels.push_back(GltfAnimationChannelData{
                .nodeIndex = channel.target_node,
                .samplerIndex = channel.sampler,
                .path = channel.target_path,
            });
        }
        outModel.animations.push_back(std::move(projected));
    }
}

void GltfLoader::processLightDefinitions(const tinygltf::Model& model)
{
    VKDEMO_CPU_SCOPE("Scene.Gltf.ProcessLights");
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
            light.intensity = convertGltfLightIntensity(
                light.type, static_cast<float>(gltfLight.intensity));
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
        light.intensity = convertGltfLightIntensity(
            light.type, readFloatValue(lightValue, "intensity", 1.0f));
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
                             bool parentVisible,
                             GltfModel& outModel) {
    VKDEMO_CPU_SCOPE("Scene.Gltf.ProcessNode");
    struct TraversalFrame {
        int nodeIndex = -1;
        int parentNodeIndex = -1;
        glm::mat4 parentTransform = glm::mat4(1.0f);
        std::size_t depth = 0;
        bool parentVisible = true;
        bool exiting = false;
    };

    const std::size_t traversalLimit = model.nodes.size();
    const std::size_t traversalWorkLimit = traversalLimit + 1u;
    std::vector<TraversalFrame> pending;
    pending.reserve(std::min<std::size_t>(traversalWorkLimit, 64u));
    pending.push_back(TraversalFrame{
        .nodeIndex = nodeIndex,
        .parentNodeIndex = parentNodeIndex,
        .parentTransform = parentTransform,
        .depth = 1,
        .parentVisible = parentVisible,
    });

    while(!pending.empty())
    {
        const TraversalFrame frame = std::move(pending.back());
        pending.pop_back();

        if(frame.nodeIndex < 0 || frame.nodeIndex >= static_cast<int>(model.nodes.size()))
        {
            m_lastError = "Invalid node index: " + std::to_string(frame.nodeIndex);
            return false;
        }
        const std::size_t sourceNodeIndex = static_cast<std::size_t>(frame.nodeIndex);
        if(sourceNodeIndex >= m_nodeVisitState.size())
        {
            m_lastError = "Node visit state out of range";
            return false;
        }

        if(frame.exiting)
        {
            if(m_nodeVisitState[sourceNodeIndex] != 1u)
            {
                m_lastError = "glTF node traversal state is inconsistent";
                return false;
            }
            m_nodeVisitState[sourceNodeIndex] = 2u;
            continue;
        }

        if(frame.depth == 0 || frame.depth > traversalLimit)
        {
            m_lastError = "glTF node graph exceeds the bounded traversal depth";
            return false;
        }
        if(frame.parentNodeIndex < -1
            || frame.parentNodeIndex >= static_cast<int>(outModel.nodes.size()))
        {
            m_lastError = "Projected parent node index is out of range";
            return false;
        }
        if(m_nodeVisitState[sourceNodeIndex] == 1u)
        {
            m_lastError = "glTF node graph contains a cycle";
            return false;
        }
        if(m_nodeVisitState[sourceNodeIndex] == 2u)
        {
            // Validation rejects duplicate parents. A completed node is still
            // expected when the scene-less fallback scans every flat node.
            continue;
        }
        m_nodeVisitState[sourceNodeIndex] = 1u;

        const auto& node = model.nodes[sourceNodeIndex];

        // Compute local transform
        glm::mat4 localTransform(1.0f);
        glm::vec3 translation(0.0f);
        glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 scale(1.0f);

        if(node.matrix.size() == 16)
        {
            // Use matrix if provided
            localTransform = glm::make_mat4(node.matrix.data());
            glm::vec3 skew(0.0f);
            glm::vec4 perspective(0.0f);
            if(!glm::decompose(localTransform, scale, rotation, translation, skew, perspective))
            {
                translation = glm::vec3(localTransform[3]);
                rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
                scale = glm::vec3(1.0f);
            }
        }
        else
        {
            // Build from TRS components
            if(node.translation.size() == 3)
            {
                translation = glm::vec3(node.translation[0], node.translation[1], node.translation[2]);
            }

            if(node.rotation.size() == 4)
            {
                // glTF uses (x, y, z, w) quaternion format
                rotation = glm::quat(
                    static_cast<float>(node.rotation[3]),  // w
                    static_cast<float>(node.rotation[0]),  // x
                    static_cast<float>(node.rotation[1]),  // y
                    static_cast<float>(node.rotation[2])   // z
                );
            }

            if(node.scale.size() == 3)
            {
                scale = glm::vec3(node.scale[0], node.scale[1], node.scale[2]);
            }

            // Compose TRS: T * R * S
            localTransform = composeTransform(translation, rotation, scale);
        }

        if(glm::length(rotation) < 0.0001f)
        {
            rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        }
        else
        {
            rotation = glm::normalize(rotation);
        }

        GltfNodeData nodeData;
        nodeData.name = node.name.empty() ? ("Node " + std::to_string(frame.nodeIndex)) : node.name;
        nodeData.parent = frame.parentNodeIndex;
        nodeData.translation = translation;
        nodeData.rotation = rotation;
        nodeData.rotationEulerDegrees = sanitizeEulerDegrees(glm::eulerAngles(rotation));
        nodeData.scale = scale;
        nodeData.localTransform = composeTransform(nodeData.translation, nodeData.rotation, nodeData.scale);
        nodeData.worldTransform = frame.parentTransform * nodeData.localTransform;
        nodeData.firstMeshIndex = static_cast<uint32_t>(outModel.meshes.size());
        nodeData.documentNodeIndex = frame.nodeIndex;
        nodeData.extensionsJson = node.extensions_json_string;
        nodeData.extrasJson = node.extras_json_string;
        nodeData.localVisible = true;
        const auto visibilityIt = node.extensions.find("KHR_node_visibility");
        if(visibilityIt != node.extensions.end() && visibilityIt->second.IsObject())
        {
            const tinygltf::Value& visible = visibilityIt->second.Get("visible");
            if(visible.IsBool())
            {
                nodeData.localVisible = visible.Get<bool>();
            }
        }
        nodeData.effectiveVisible = frame.parentVisible && nodeData.localVisible;

        const int currentNodeIndex = static_cast<int>(outModel.nodes.size());
        outModel.nodes.push_back(nodeData);
        if(frame.parentNodeIndex >= 0)
        {
            outModel.nodes[static_cast<std::size_t>(frame.parentNodeIndex)].children.push_back(currentNodeIndex);
        }
        else
        {
            outModel.rootNodes.push_back(currentNodeIndex);
        }

        if(node.camera >= 0 && static_cast<size_t>(node.camera) < model.cameras.size())
        {
            const tinygltf::Camera& gltfCamera = model.cameras[static_cast<size_t>(node.camera)];
            SceneCamera camera{};
            camera.name = gltfCamera.name.empty() ? nodeData.name : gltfCamera.name;
            camera.nodeIndex = currentNodeIndex;
            if(gltfCamera.type == "orthographic")
            {
                camera.type = SceneCameraType::orthographic;
                camera.horizontalMagnification = static_cast<float>(gltfCamera.orthographic.xmag);
                camera.verticalMagnification = static_cast<float>(gltfCamera.orthographic.ymag);
                camera.nearPlane = static_cast<float>(gltfCamera.orthographic.znear);
                camera.farPlane = static_cast<float>(gltfCamera.orthographic.zfar);
            }
            else
            {
                camera.type = SceneCameraType::perspective;
                camera.aspectRatio = static_cast<float>(gltfCamera.perspective.aspectRatio);
                camera.verticalFieldOfViewRadians = static_cast<float>(gltfCamera.perspective.yfov);
                camera.nearPlane = static_cast<float>(gltfCamera.perspective.znear);
                camera.farPlane = static_cast<float>(gltfCamera.perspective.zfar);
            }
            outModel.cameras.push_back(std::move(camera));
            outModel.cameraDocumentIndices.push_back(node.camera);
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
            outModel.lightDocumentIndices.push_back(lightIndex);
        }

        // Process mesh if present
        if(node.mesh >= 0)
        {
            if(!processMesh(model,
                            node.mesh,
                            frame.nodeIndex,
                            outModel.nodes[static_cast<std::size_t>(currentNodeIndex)].worldTransform,
                            outModel.nodes[static_cast<std::size_t>(currentNodeIndex)].effectiveVisible,
                            outModel))
            {
                return false;
            }
        }
        outModel.nodes[static_cast<std::size_t>(currentNodeIndex)].meshCount =
            static_cast<uint32_t>(outModel.meshes.size())
            - outModel.nodes[static_cast<std::size_t>(currentNodeIndex)].firstMeshIndex;

        if(node.children.size() >= traversalWorkLimit
            || pending.size() > traversalWorkLimit - node.children.size() - 1u)
        {
            m_lastError = "glTF node graph exceeds the bounded traversal work budget";
            return false;
        }

        pending.push_back(TraversalFrame{
            .nodeIndex = frame.nodeIndex,
            .depth = frame.depth,
            .exiting = true,
        });
        const glm::mat4 childParentTransform =
            outModel.nodes[static_cast<std::size_t>(currentNodeIndex)].worldTransform;
        const bool childParentVisible =
            outModel.nodes[static_cast<std::size_t>(currentNodeIndex)].effectiveVisible;
        // LIFO insertion in reverse preserves the recursive child preorder.
        for(auto child = node.children.rbegin(); child != node.children.rend(); ++child)
        {
            pending.push_back(TraversalFrame{
                .nodeIndex = *child,
                .parentNodeIndex = currentNodeIndex,
                .parentTransform = childParentTransform,
                .depth = frame.depth + 1u,
                .parentVisible = childParentVisible,
            });
        }
    }

    return true;
}

bool GltfLoader::processMesh(const tinygltf::Model& model,
                              int meshIndex,
                              int nodeIndex,
                              const glm::mat4& transform,
                              bool effectiveVisible,
                              GltfModel& outModel) {
    VKDEMO_CPU_SCOPE("Scene.Gltf.ProcessMesh");
    if (meshIndex < 0 || meshIndex >= static_cast<int>(model.meshes.size())) {
        m_lastError = "Invalid mesh index: " + std::to_string(meshIndex);
        return false;
    }

    const auto& mesh = model.meshes[meshIndex];

    // Process each primitive
    for (std::size_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex) {
        const auto& primitive = mesh.primitives[primitiveIndex];
        GltfMeshData meshData;
        meshData.transform = transform;
        meshData.materialIndex = primitive.material;
        meshData.documentNodeIndex = nodeIndex;
        meshData.documentMeshIndex = meshIndex;
        meshData.documentPrimitiveIndex = static_cast<int>(primitiveIndex);
        meshData.effectiveVisible = effectiveVisible;
        meshData.extensionsJson = primitive.extensions_json_string;
        meshData.extrasJson = primitive.extras_json_string;

        const auto variantsIt = primitive.extensions.find("KHR_materials_variants");
        if(variantsIt != primitive.extensions.end() && variantsIt->second.IsObject())
        {
            const tinygltf::Value& mappings = variantsIt->second.Get("mappings");
            if(mappings.IsArray())
            {
                for(std::size_t mappingIndex = 0; mappingIndex < mappings.ArrayLen(); ++mappingIndex)
                {
                    const tinygltf::Value& mapping = mappings.Get(static_cast<int>(mappingIndex));
                    if(!mapping.IsObject())
                    {
                        continue;
                    }
                    GltfVariantBinding binding;
                    if(readTinyGltfInt(mapping.Get("material"), binding.materialIndex))
                    {
                        const tinygltf::Value& variants = mapping.Get("variants");
                        if(variants.IsArray())
                        {
                            for(std::size_t variantIndex = 0; variantIndex < variants.ArrayLen(); ++variantIndex)
                            {
                                int value = -1;
                                if(readTinyGltfInt(variants.Get(static_cast<int>(variantIndex)), value)
                                   && value >= 0)
                                {
                                    binding.variantIndices.push_back(static_cast<uint32_t>(value));
                                }
                            }
                        }
                        meshData.variantBindings.push_back(std::move(binding));
                    }
                }
            }
        }

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
    VKDEMO_CPU_SCOPE("Scene.Gltf.ProcessMaterials");
    constexpr uint8_t kColorUsage = 1u << 0u;
    constexpr uint8_t kLinearUsage = 1u << 1u;
    const auto markTextureUsage = [&](int textureIndex, uint8_t usageBit)
    {
        if(textureIndex >= 0 && static_cast<size_t>(textureIndex) < outModel.textures.size())
        {
            outModel.textures[static_cast<size_t>(textureIndex)].usage |= usageBit;
        }
    };
    outModel.materials.reserve(model.materials.size());

    for (const auto& mat : model.materials) {
        GltfMaterialData data;
        data.name = mat.name;
        data.doubleSided = mat.doubleSided;
        data.extensionsJson = mat.extensions_json_string;
        data.extrasJson = mat.extras_json_string;

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
            markTextureUsage(pbr.baseColorTexture.index, kColorUsage);
            data.baseColorTexture = resolveTextureSourceIndex(model, pbr.baseColorTexture.index);
        }

        // Metallic-Roughness
        data.metallicFactor = static_cast<float>(pbr.metallicFactor);
        data.roughnessFactor = static_cast<float>(pbr.roughnessFactor);
        if (pbr.metallicRoughnessTexture.index >= 0) {
            markTextureUsage(pbr.metallicRoughnessTexture.index, kLinearUsage);
            data.metallicRoughnessTexture = resolveTextureSourceIndex(model, pbr.metallicRoughnessTexture.index);
        }

        // Normal map
        if (mat.normalTexture.index >= 0) {
            markTextureUsage(mat.normalTexture.index, kLinearUsage);
            data.normalTexture = resolveTextureSourceIndex(model, mat.normalTexture.index);
            data.normalScale = static_cast<float>(mat.normalTexture.scale);
        }

        // Occlusion
        if (mat.occlusionTexture.index >= 0) {
            markTextureUsage(mat.occlusionTexture.index, kLinearUsage);
            data.occlusionTexture = resolveTextureSourceIndex(model, mat.occlusionTexture.index);
            data.occlusionStrength = static_cast<float>(mat.occlusionTexture.strength);
        }

        // Emissive
        data.emissiveFactor = glm::vec3(
            static_cast<float>(mat.emissiveFactor[0]),
            static_cast<float>(mat.emissiveFactor[1]),
            static_cast<float>(mat.emissiveFactor[2])
        );
        const auto emissiveStrengthIt = mat.extensions.find("KHR_materials_emissive_strength");
        if(emissiveStrengthIt != mat.extensions.end() && emissiveStrengthIt->second.IsObject())
        {
            const float parsedStrength = readFloatExtensionFactor(
                emissiveStrengthIt->second, "emissiveStrength", 1.0f);
            const float emissiveStrength = std::isfinite(parsedStrength)
                ? std::max(parsedStrength, 0.0f) : 1.0f;
            data.emissiveFactor *= emissiveStrength;
        }
        if (mat.emissiveTexture.index >= 0) {
            markTextureUsage(mat.emissiveTexture.index, kColorUsage);
            data.emissiveTexture = resolveTextureSourceIndex(model, mat.emissiveTexture.index);
        }

        const auto specGlossIt = mat.extensions.find("KHR_materials_pbrSpecularGlossiness");
        if(specGlossIt != mat.extensions.end() && specGlossIt->second.IsObject())
        {
            const tinygltf::Value& specGloss = specGlossIt->second;
            int diffuseTexture = -1;
            if(readExtensionTextureIndex(model, specGloss, "diffuseTexture", diffuseTexture))
            {
                const tinygltf::Value& textureInfo = specGloss.Get("diffuseTexture");
                int textureIndex = -1;
                if(textureInfo.IsObject()
                   && readTinyGltfInt(textureInfo.Get("index"), textureIndex))
                {
                    markTextureUsage(textureIndex, kColorUsage);
                }
                data.baseColorTexture = diffuseTexture;
            }
            data.baseColorFactor = readVec4ExtensionFactor(specGloss, "diffuseFactor", data.baseColorFactor);

            int specularGlossinessTexture = -1;
            if(readExtensionTextureIndex(model, specGloss, "specularGlossinessTexture", specularGlossinessTexture))
            {
                const tinygltf::Value& textureInfo = specGloss.Get("specularGlossinessTexture");
                int textureIndex = -1;
                if(textureInfo.IsObject()
                   && readTinyGltfInt(textureInfo.Get("index"), textureIndex))
                {
                    markTextureUsage(textureIndex, kLinearUsage);
                }
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

    const auto variantsIt = model.extensions.find("KHR_materials_variants");
    if(variantsIt != model.extensions.end() && variantsIt->second.IsObject())
    {
        const tinygltf::Value& variants = variantsIt->second.Get("variants");
        if(variants.IsArray())
        {
            outModel.materialVariantNames.reserve(variants.ArrayLen());
            for(std::size_t variantIndex = 0; variantIndex < variants.ArrayLen(); ++variantIndex)
            {
                const tinygltf::Value& variant = variants.Get(static_cast<int>(variantIndex));
                outModel.materialVariantNames.push_back(
                    readStringValue(variant, "name", "Variant " + std::to_string(variantIndex)));
            }
        }
    }
}

void GltfLoader::processImages(const tinygltf::Model& model, const std::filesystem::path& sourcePath, GltfModel& outModel) {
    VKDEMO_CPU_SCOPE("Scene.Gltf.ProcessImages");
    (void)sourcePath;
    std::vector<GltfImageData> decodedImages;
    try
    {
        decodedImages.resize(model.images.size());
    }
    catch(...)
    {
        m_lastError = "Failed to allocate the glTF image table";
        return;
    }

    for (size_t imageIndex = 0; imageIndex < model.images.size(); ++imageIndex) {
        const auto& image = model.images[imageIndex];
        const bool isKtx2 = image.mimeType == "image/ktx2"
                         || hasExtension(std::filesystem::path(image.uri), ".ktx2")
                         || hasKtx2Identifier(image.image.data(), image.image.size());

        GltfImageData imageData;
        imageData.width = image.width;
        imageData.height = image.height;
        imageData.channels = 4;
        imageData.uri = image.uri;
        imageData.mimeType = image.mimeType;
        imageData.isKtx2 = isKtx2;
        imageData.documentImageIndex = static_cast<int>(imageIndex);

        if(imageData.uri.empty() && !image.name.empty())
        {
            imageData.uri = image.name;
        }

        if(isKtx2)
        {
            // KTX2 remains a compressed container.  Do not send it through
            // stb_image or expand it into a raw RGBA payload here.
            if(image.image.size() > kMaxReasonableImageBytes)
            {
                m_lastError = "KTX2 image[" + std::to_string(imageIndex)
                    + "] exceeds the bounded container payload";
                return;
            }
            imageData.ktx2Data = image.image;
            if(image.width < 0 || image.height < 0 || image.component < 0
               || static_cast<size_t>(image.width) > kMaxReasonableImageDimension
               || static_cast<size_t>(image.height) > kMaxReasonableImageDimension)
            {
                decodedImages[imageIndex] = std::move(imageData);
                continue;
            }
        }
        else
        {
            const bool hasPngMagic = hasPngIdentifier(image.image.data(), image.image.size());
            const bool hasJpegMagic = hasJpegIdentifier(image.image.data(), image.image.size());
            const EncodedImageKind declaredKind = declaredEncodedImageKind(image);
            if(image.as_is || hasPngMagic || hasJpegMagic)
            {
                std::string decodeError;
                if(!decodeEncodedImageRgba8(image, imageData, decodeError))
                {
                    m_lastError = "Failed to decode encoded image["
                        + std::to_string(imageIndex) + "]: " + decodeError;
                    return;
                }
            }
            else if(declaredKind == EncodedImageKind::png
                    || declaredKind == EncodedImageKind::jpeg)
            {
                // An image declared as PNG/JPEG without its matching magic is
                // not allowed to fall through to another stb format. Keep
                // TinyGLTF's opaque unknown-format placeholder only when its
                // metadata is already invalid; otherwise reject it.
                if(image.width < 0 || image.height < 0 || image.component < 0)
                {
                    decodedImages[imageIndex] = std::move(imageData);
                    continue;
                }
                m_lastError = "Encoded image[" + std::to_string(imageIndex)
                    + "] declaration does not match a PNG/JPEG payload";
                return;
            }
            else
            {
                // TinyGLTF uses negative metadata for unrecognized as-is
                // image formats. Preserve the historical empty runtime
                // placeholder; recognized PNG/JPEG payloads take the strict
                // path above.
                if(image.width < 0 || image.height < 0 || image.component < 0)
                {
                    decodedImages[imageIndex] = std::move(imageData);
                    continue;
                }

                // Normalize to RGBA8 so the upload path can generate mipmaps reliably.
                if(image.width <= 0 || image.height <= 0
                   || static_cast<size_t>(image.width) > kMaxReasonableImageDimension
                   || static_cast<size_t>(image.height) > kMaxReasonableImageDimension
                   || image.component < 1 || image.component > 4
                   || (image.bits > 0 && image.bits != 8)
                   || !expandToRgba8(image.image, image.width, image.height,
                                     image.component, imageData.pixels))
                {
                    m_lastError = "Invalid decoded image[" + std::to_string(imageIndex)
                        + "] dimensions, channels, or payload";
                    return;
                }
            }
        }

        decodedImages[imageIndex] = std::move(imageData);
    }

    outModel.images = std::move(decodedImages);
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
