#include <mgif/scene_editor/SceneMerger.h>

#include <mgif/scene_editor/GltfValidator.h>

#include <tiny_gltf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>
#include <vector>

namespace demo::editor {
namespace detail {

// Keep TinyGLTF and the atomic document publication operation private to this
// translation unit. The public merger only exposes typed IDs and status data.
class SceneMergerAccess final
{
public:
	[[nodiscard]] static const tinygltf::Model& model(const GltfDocument& document) noexcept
	{
		return document.authorityModel();
	}

	[[nodiscard]] static GltfValidationReport validateCandidate(
		std::unique_ptr<tinygltf::Model>& candidate, const EditorLimits& limits)
	{
		GltfDocument temporary(std::move(candidate), {}, GltfDocumentFormat::ascii, {});
		GltfValidationReport report = GltfValidator::validate(temporary, limits);
		candidate = temporary.takeAuthorityModel();
		return report;
	}

	[[nodiscard]] static MutationCommitStatus commit(
		GltfDocument& document,
		std::unique_ptr<tinygltf::Model> candidate,
		MutationRegistryPlan& plan,
		MutationPrePublishCallback callback,
		void* context) noexcept
	{
		return document.commitMutation(std::move(candidate), plan, callback, context);
	}
};

} // namespace detail

namespace {

using Model = tinygltf::Model;
using Value = tinygltf::Value;

struct Offsets final
{
	std::size_t node{0};
	std::size_t mesh{0};
	std::size_t material{0};
	std::size_t texture{0};
	std::size_t image{0};
	std::size_t sampler{0};
	std::size_t accessor{0};
	std::size_t bufferView{0};
	std::size_t buffer{0};
	std::size_t skin{0};
	std::size_t camera{0};
	std::size_t animation{0};
	std::size_t light{0};
	std::size_t audioEmitter{0};
	std::size_t audioSource{0};
	std::size_t variant{0};
};

[[nodiscard]] bool checkedIndexOffset(int& index, std::size_t offset) noexcept
{
	// TinyGLTF leaves several inactive optional sub-objects at debug-fill
	// values. Their owning flags are checked by callers, so all negative typed
	// values remain non-references here; opaque JSON indices are stricter below.
	if(index < 0) return true;
	if(offset > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
	const std::size_t value = static_cast<std::size_t>(index);
	if(value > static_cast<std::size_t>(std::numeric_limits<int>::max()) - offset)
		return false;
	index += static_cast<int>(offset);
	return true;
}

[[nodiscard]] bool remapIndex(int& index, std::size_t offset,
	std::size_t sourceCount, bool optional = true) noexcept
{
	if(index < 0)
		return optional ? index == -1 : false;
	if(static_cast<std::size_t>(index) >= sourceCount) return false;
	return checkedIndexOffset(index, offset);
}

[[nodiscard]] bool valueIsInteger(const Value& value, int& result) noexcept
{
	if(!value.IsInt()) return false;
	result = value.GetNumberAsInt();
	return true;
}

[[nodiscard]] bool remapIndexValue(Value& value, std::size_t offset,
	std::size_t sourceCount, bool optional = false)
{
	int index = 0;
	if(!valueIsInteger(value, index) || !remapIndex(index, offset, sourceCount, optional))
		return false;
	value = Value(index);
	return true;
}

[[nodiscard]] bool offsetIndexValue(Value& value, std::size_t offset,
	std::size_t sourceCount)
{
	return remapIndexValue(value, offset, sourceCount);
}

[[nodiscard]] bool appendSize(std::size_t left, std::size_t right, std::size_t& result) noexcept
{
	if(right > std::numeric_limits<std::size_t>::max() - left) return false;
	result = left + right;
	return true;
}

[[nodiscard]] std::size_t variantCount(const Model& model) noexcept
{
	const auto extension = model.extensions.find("KHR_materials_variants");
	if(extension == model.extensions.end() || !extension->second.IsObject()
		|| !extension->second.Has("variants")
		|| !extension->second.Get("variants").IsArray()) return 0u;
	return extension->second.Get("variants").ArrayLen();
}

[[nodiscard]] bool remapTextureInfo(Value& value, std::size_t offset,
	std::size_t sourceTextureCount)
{
	if(!value.IsObject()) return false;
	auto& object = value.Get<Value::Object>();
	bool found = false;
	for(auto& member : object)
	{
		if(member.first != "index") continue;
		found = true;
		if(!offsetIndexValue(member.second, offset, sourceTextureCount)) return false;
	}
	return found;
}

[[nodiscard]] bool remapKnownMaterialExtensionTextures(
	tinygltf::Material& material, const Offsets& offsets,
	std::size_t sourceTextureCount)
{
	static constexpr std::array<std::string_view, 22> textureFields{
		"baseColorTexture", "metallicRoughnessTexture", "normalTexture",
		"occlusionTexture", "emissiveTexture", "diffuseTexture",
		"specularGlossinessTexture", "clearcoatTexture",
		"clearcoatRoughnessTexture", "clearcoatNormalTexture",
		"sheenColorTexture", "sheenRoughnessTexture", "specularTexture",
		"specularColorTexture", "transmissionTexture", "thicknessTexture",
		"iridescenceTexture", "iridescenceThicknessTexture",
		"anisotropyTexture", "diffuseTransmissionTexture",
		"diffuseTransmissionColorTexture", "bumpTexture"};
	static constexpr std::array<std::string_view, 12> knownExtensions{
		"KHR_materials_pbrSpecularGlossiness", "KHR_materials_clearcoat",
		"KHR_materials_sheen", "KHR_materials_specular",
		"KHR_materials_transmission", "KHR_materials_volume",
		"KHR_materials_iridescence", "KHR_materials_anisotropy",
		"KHR_materials_diffuse_transmission", "KHR_materials_dispersion",
		"KHR_materials_emissive_strength", "EXT_materials_bump"};

	for(const std::string_view extensionName : knownExtensions)
	{
		auto extension = material.extensions.find(std::string(extensionName));
		if(extension == material.extensions.end()) continue;
		if(!extension->second.IsObject()) return false;
		for(auto& member : extension->second.Get<Value::Object>())
		{
			if(std::find(textureFields.begin(), textureFields.end(), member.first)
				== textureFields.end()) continue;
			if(!remapTextureInfo(member.second, offsets.texture, sourceTextureCount)) return false;
		}
	}
	return true;
}

[[nodiscard]] bool remapKnownTextureExtensions(tinygltf::Texture& texture,
	                                           const Offsets& offsets,
	                                           std::size_t sourceImageCount)
{
	for(const auto& name : {std::string_view{"KHR_texture_basisu"},
	                        std::string_view{"EXT_texture_webp"},
	                        std::string_view{"EXT_texture_avif"},
	                        std::string_view{"MSFT_texture_dds"}})
	{
		auto it = texture.extensions.find(std::string(name));
		if(it == texture.extensions.end()) continue;
		if(!it->second.IsObject()) return false;
		auto& object = it->second.Get<Value::Object>();
		bool found = false;
		for(auto& member : object)
		{
			if(member.first == "source")
			{
				found = true;
				if(!offsetIndexValue(member.second, offsets.image, sourceImageCount)) return false;
			}
		}
		if(!found) return false;
	}
	return true;
}

[[nodiscard]] bool remapLegacyMaterialParameters(
	tinygltf::ParameterMap& parameters, std::size_t offset,
	std::size_t sourceTextureCount)
{
	for(auto& [name, parameter] : parameters)
	{
		(void)name;
		auto index = parameter.json_double_value.find("index");
		if(index == parameter.json_double_value.end()) continue;
		if(!std::isfinite(index->second) || std::floor(index->second) != index->second
			|| index->second < 0.0
			|| index->second > static_cast<double>(std::numeric_limits<int>::max()))
			return false;
		int remapped = static_cast<int>(index->second);
		if(!remapIndex(remapped, offset, sourceTextureCount, false)) return false;
		index->second = static_cast<double>(remapped);
	}
	return true;
}

[[nodiscard]] bool remapMaterial(tinygltf::Material& material,
	                              const Offsets& offsets,
	                              std::size_t sourceTextureCount,
	                              std::size_t sourceMaterialCount)
{
	if(!checkedIndexOffset(material.pbrMetallicRoughness.baseColorTexture.index, offsets.texture)
		|| !checkedIndexOffset(material.pbrMetallicRoughness.metallicRoughnessTexture.index, offsets.texture)
		|| !checkedIndexOffset(material.normalTexture.index, offsets.texture)
		|| !checkedIndexOffset(material.occlusionTexture.index, offsets.texture)
		|| !checkedIndexOffset(material.emissiveTexture.index, offsets.texture))
		return false;
	if(!remapLegacyMaterialParameters(material.values, offsets.texture, sourceTextureCount)
		|| !remapLegacyMaterialParameters(
			material.additionalValues, offsets.texture, sourceTextureCount)) return false;
	if(!remapKnownMaterialExtensionTextures(material, offsets, sourceTextureCount)) return false;
	for(int& lod : material.lods)
		if(!remapIndex(lod, offsets.material, sourceMaterialCount)) return false;
	if(auto it = material.extensions.find("MSFT_lod");
		it != material.extensions.end())
	{
		if(!it->second.IsObject() || !it->second.Has("ids")
			|| !it->second.Get("ids").IsArray()) return false;
		auto& values = it->second.Get<Value::Object>()["ids"].Get<Value::Array>();
		if(values.size() != material.lods.size()) return false;
		for(std::size_t index = 0; index < values.size(); ++index)
			if(!offsetIndexValue(values[index], offsets.material, sourceMaterialCount)
				|| values[index].GetNumberAsInt() != material.lods[index]) return false;
	}
	return true;
}

[[nodiscard]] bool remapPrimitive(tinygltf::Primitive& primitive,
                               const Offsets& offsets,
                               const Model& source)
{
	if(!checkedIndexOffset(primitive.material, offsets.material)
		|| !checkedIndexOffset(primitive.indices, offsets.accessor)) return false;
	for(auto& attribute : primitive.attributes)
		if(!checkedIndexOffset(attribute.second, offsets.accessor)) return false;
	for(auto& target : primitive.targets)
		for(auto& attribute : target)
			if(!checkedIndexOffset(attribute.second, offsets.accessor)) return false;

	if(auto it = primitive.extensions.find("KHR_draco_mesh_compression");
		it != primitive.extensions.end() && it->second.IsObject())
	{
		auto& object = it->second.Get<Value::Object>();
		bool found = false;
		for(auto& member : object)
			if(member.first == "bufferView")
			{
				found = true;
				if(!offsetIndexValue(member.second, offsets.bufferView, source.bufferViews.size())) return false;
			}
		if(!found) return false;
	}
	else if(primitive.extensions.find("KHR_draco_mesh_compression")
		!= primitive.extensions.end()) return false;
	if(auto it = primitive.extensions.find("KHR_materials_variants");
		it != primitive.extensions.end() && it->second.IsObject())
	{
		auto& object = it->second.Get<Value::Object>();
		bool found = false;
		for(auto& member : object)
		{
			if(member.first != "mappings") continue;
			found = true;
			if(!member.second.IsArray()) return false;
			for(Value& mapping : member.second.Get<Value::Array>())
			{
				if(!mapping.IsObject()) return false;
				bool materialFound = false;
				bool variantsFound = false;
				for(auto& field : mapping.Get<Value::Object>())
				{
					if(field.first == "material")
					{
						materialFound = true;
						if(!offsetIndexValue(field.second, offsets.material, source.materials.size())) return false;
					}
					else if(field.first == "variants")
					{
						variantsFound = true;
						if(!field.second.IsArray()) return false;
						for(Value& variant : field.second.Get<Value::Array>())
						{
							if(!offsetIndexValue(variant, offsets.variant,
								variantCount(source))) return false;
						}
					}
				}
				if(!materialFound || !variantsFound) return false;
			}
		}
		if(!found) return false;
	}
	else if(primitive.extensions.find("KHR_materials_variants")
		!= primitive.extensions.end()) return false;
	return true;
}

[[nodiscard]] bool remapNode(tinygltf::Node& node, const Offsets& offsets,
	const Model& source)
{
	for(int& child : node.children)
		if(!checkedIndexOffset(child, offsets.node)) return false;
	if(!checkedIndexOffset(node.mesh, offsets.mesh)
		|| !checkedIndexOffset(node.camera, offsets.camera)
		|| !checkedIndexOffset(node.skin, offsets.skin)
		|| !checkedIndexOffset(node.light, offsets.light)
		|| !checkedIndexOffset(node.emitter, offsets.audioEmitter)) return false;
	for(int& lod : node.lods)
		if(!checkedIndexOffset(lod, offsets.node)) return false;
	if(auto it = node.extensions.find("KHR_lights_punctual");
		it != node.extensions.end() && it->second.IsObject())
	{
		auto& object = it->second.Get<Value::Object>();
		bool found = false;
		for(auto& member : object)
			if(member.first == "light")
			{
				found = true;
				if(!offsetIndexValue(member.second, offsets.light, source.lights.size())
					|| member.second.GetNumberAsInt() != node.light) return false;
			}
		if(!found) return false;
	}
	else if(node.extensions.find("KHR_lights_punctual") != node.extensions.end()) return false;
	if(auto it = node.extensions.find("KHR_audio");
		it != node.extensions.end() && it->second.IsObject())
	{
		auto& object = it->second.Get<Value::Object>();
		bool found = false;
		for(auto& member : object)
			if(member.first == "emitter")
			{
				found = true;
				if(!offsetIndexValue(member.second, offsets.audioEmitter, source.audioEmitters.size())
					|| member.second.GetNumberAsInt() != node.emitter) return false;
			}
		if(!found) return false;
	}
	else if(node.extensions.find("KHR_audio") != node.extensions.end()) return false;
	if(auto it = node.extensions.find("EXT_mesh_gpu_instancing");
		it != node.extensions.end() && it->second.IsObject())
	{
		auto& object = it->second.Get<Value::Object>();
		bool found = false;
		for(auto& member : object)
		{
			if(member.first != "attributes") continue;
			found = true;
			if(!member.second.IsObject()) return false;
			for(auto& attribute : member.second.Get<Value::Object>())
				if(!offsetIndexValue(attribute.second, offsets.accessor, source.accessors.size())) return false;
		}
		if(!found) return false;
	}
	else if(node.extensions.find("EXT_mesh_gpu_instancing") != node.extensions.end()) return false;
	if(auto it = node.extensions.find("MSFT_lod");
		it != node.extensions.end() && it->second.IsObject())
	{
		auto& object = it->second.Get<Value::Object>();
		bool found = false;
		for(auto& member : object)
			if(member.first == "ids")
			{
				found = true;
				if(!member.second.IsArray()) return false;
				auto& values = member.second.Get<Value::Array>();
				if(values.size() != node.lods.size()) return false;
				for(std::size_t index = 0; index < values.size(); ++index)
					if(!offsetIndexValue(values[index], offsets.node, source.nodes.size())
						|| values[index].GetNumberAsInt() != node.lods[index]) return false;
			}
		if(!found) return false;
	}
	else if(node.extensions.find("MSFT_lod") != node.extensions.end()) return false;
	return true;
}

[[nodiscard]] bool remapSceneExtensions(tinygltf::Scene& scene,
	const Offsets& offsets, const Model& source)
{
	for(int& emitter : scene.audioEmitters)
		if(!checkedIndexOffset(emitter, offsets.audioEmitter)) return false;
	const auto extension = scene.extensions.find("KHR_audio");
	if(extension == scene.extensions.end()) return true;
	if(!extension->second.IsObject() || !extension->second.Has("emitters")
		|| !extension->second.Get("emitters").IsArray()) return false;
	auto& values = extension->second.Get<Value::Object>()["emitters"].Get<Value::Array>();
	if(values.size() != scene.audioEmitters.size()) return false;
	for(std::size_t index = 0; index < values.size(); ++index)
		if(!offsetIndexValue(values[index], offsets.audioEmitter, source.audioEmitters.size())
			|| values[index].GetNumberAsInt() != scene.audioEmitters[index]) return false;
	return true;
}

[[nodiscard]] bool decodeHexByte(char high, char low, unsigned char& value) noexcept
{
	const auto digit = [](char c) noexcept -> int
	{
		if(c >= '0' && c <= '9') return c - '0';
		if(c >= 'a' && c <= 'f') return c - 'a' + 10;
		if(c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	};
	const int h = digit(high);
	const int l = digit(low);
	if(h < 0 || l < 0) return false;
	value = static_cast<unsigned char>((h << 4) | l);
	return true;
}

[[nodiscard]] bool decodeAudioDataUri(std::string_view uri,
	std::vector<unsigned char>& bytes, std::string& mimeType)
{
	if(!uri.starts_with("data:")) return false;
	const std::size_t comma = uri.find(',');
	if(comma == std::string_view::npos || comma <= 5u) return false;
	const std::string_view header = uri.substr(5u, comma - 5u);
	const std::string_view payload = uri.substr(comma + 1u);
	const std::size_t semicolon = header.find(';');
	mimeType = std::string(header.substr(0u, semicolon == std::string_view::npos
		? header.size() : semicolon));
	if(mimeType.empty()) mimeType = "application/octet-stream";
	const bool base64 = header.find(";base64") != std::string_view::npos;
	if(base64)
	{
		std::array<int, 4> quartet{};
		std::size_t count = 0;
		bool padded = false;
		const auto emit = [&]() -> bool
		{
			if(count != 4u || quartet[0] < 0 || quartet[1] < 0) return false;
			bytes.push_back(static_cast<unsigned char>((quartet[0] << 2) | (quartet[1] >> 4)));
			if(quartet[2] >= 0)
			{
				bytes.push_back(static_cast<unsigned char>(
					((quartet[1] & 0x0f) << 4) | (quartet[2] >> 2)));
				if(quartet[3] >= 0)
					bytes.push_back(static_cast<unsigned char>(
						((quartet[2] & 0x03) << 6) | quartet[3]));
				else padded = true;
			}
			else
			{
				if(quartet[3] >= 0) return false;
				padded = true;
			}
			count = 0;
			return true;
		};
		for(const char c : payload)
		{
			if(c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
			if(padded) return false;
			int digit = -1;
			if(c >= 'A' && c <= 'Z') digit = c - 'A';
			else if(c >= 'a' && c <= 'z') digit = c - 'a' + 26;
			else if(c >= '0' && c <= '9') digit = c - '0' + 52;
			else if(c == '+') digit = 62;
			else if(c == '/') digit = 63;
			else if(c == '=') digit = -2;
			else return false;
			quartet[count++] = digit == -2 ? -1 : digit;
			if(count == 4u && !emit()) return false;
		}
		return count == 0u && !bytes.empty();
	}
	for(std::size_t index = 0; index < payload.size(); ++index)
	{
		if(payload[index] == '%')
		{
			if(index + 2u >= payload.size()) return false;
			unsigned char decoded = 0;
			if(!decodeHexByte(payload[index + 1u], payload[index + 2u], decoded)) return false;
			bytes.push_back(decoded);
			index += 2u;
		}
		else bytes.push_back(static_cast<unsigned char>(payload[index]));
	}
	return !bytes.empty();
}

[[nodiscard]] std::string decodeUriPath(std::string_view uri, bool& valid)
{
	valid = true;
	std::string decoded;
	decoded.reserve(uri.size());
	for(std::size_t index = 0; index < uri.size(); ++index)
	{
		if(uri[index] != '%')
		{
			decoded.push_back(uri[index]);
			continue;
		}
		if(index + 2u >= uri.size()) { valid = false; return {}; }
		unsigned char byte = 0;
		if(!decodeHexByte(uri[index + 1u], uri[index + 2u], byte))
		{
			valid = false;
			return {};
		}
		decoded.push_back(static_cast<char>(byte));
		index += 2u;
	}
	return decoded;
}

[[nodiscard]] bool pathContained(const std::filesystem::path& root,
	const std::filesystem::path& candidate)
{
	const std::filesystem::path relative = candidate.lexically_relative(root);
	if(relative.empty() || relative == ".") return false;
	for(const auto& component : relative)
		if(component == ".." || component == "../") return false;
	return !relative.has_root_path();
}

[[nodiscard]] std::string inferredAudioMime(std::string_view uri)
{
	const std::string extension = std::filesystem::path(std::string(uri)).extension().string();
	if(extension == ".mp3" || extension == ".mpeg") return "audio/mpeg";
	if(extension == ".ogg" || extension == ".oga") return "audio/ogg";
	if(extension == ".wav") return "audio/wav";
	if(extension == ".m4a" || extension == ".mp4") return "audio/mp4";
	return "application/octet-stream";
}

[[nodiscard]] bool readExternalAudio(const GltfDocument& incoming,
	std::string_view uri, const EditorLimits& limits,
	std::vector<unsigned char>& bytes, std::string& mimeType)
{
	if(uri.starts_with("data:")) return decodeAudioDataUri(uri, bytes, mimeType);
	bool uriValid = false;
	const std::string decoded = decodeUriPath(uri, uriValid);
	if(!uriValid || decoded.empty() || decoded.find(':') != std::string::npos
		|| decoded.find('?') != std::string::npos || decoded.find('#') != std::string::npos
		|| decoded.find('\0') != std::string::npos) return false;
	const std::filesystem::path source = incoming.sourcePath();
	if(source.empty()) return false;
	const std::filesystem::path root = source.parent_path();
	const std::filesystem::path relative(decoded);
	if(relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) return false;
	std::error_code ec;
	const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(root, ec);
	if(ec) return false;
	const std::filesystem::path resolved = std::filesystem::canonical(root / relative, ec);
	if(ec || !pathContained(canonicalRoot, resolved)) return false;
	const std::uintmax_t size = std::filesystem::file_size(resolved, ec);
	if(ec || size == 0u || size > limits.maxExternalFileBytes
		|| size > limits.maxBufferBytes
		|| size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())
		|| size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) return false;
	bytes.resize(static_cast<std::size_t>(size));
	std::ifstream stream(resolved, std::ios::binary);
	if(!stream) return false;
	stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	if(stream.gcount() != static_cast<std::streamsize>(bytes.size()) || stream.bad())
	{
		bytes.clear();
		return false;
	}
	mimeType = inferredAudioMime(uri);
	return true;
}

[[nodiscard]] bool stageExternalAudio(Model& model, const GltfDocument& incoming,
	const EditorLimits& limits)
{
	std::uint64_t stagedBytes = 0;
	for(std::size_t index = 0; index < model.audioSources.size(); ++index)
	{
		auto& source = model.audioSources[index];
		if(source.uri.empty()) continue;
		if(source.bufferView >= 0) return false;
		std::vector<unsigned char> bytes;
		std::string mimeType;
		if(!readExternalAudio(incoming, source.uri, limits, bytes, mimeType)
			|| bytes.size() > limits.maxExternalFileBytes
			|| bytes.size() > limits.maxBufferBytes
			|| stagedBytes > std::numeric_limits<std::uint64_t>::max() - bytes.size()) return false;
		stagedBytes += static_cast<std::uint64_t>(bytes.size());
		if(stagedBytes > limits.maxAuthorityPayloadBytes) return false;
		const std::size_t bufferIndex = model.buffers.size();
		const std::size_t viewIndex = model.bufferViews.size();
		if(bufferIndex > static_cast<std::size_t>(std::numeric_limits<int>::max())
			|| viewIndex > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
		tinygltf::Buffer buffer;
		buffer.name = "MGIF staged audio " + std::to_string(index);
		buffer.data = std::move(bytes);
		model.buffers.push_back(std::move(buffer));
		tinygltf::BufferView view;
		view.buffer = static_cast<int>(bufferIndex);
		view.byteOffset = 0;
		view.byteLength = model.buffers.back().data.size();
		model.bufferViews.push_back(std::move(view));
		source.bufferView = static_cast<int>(viewIndex);
		source.uri.clear();
		if(source.mimeType.empty()) source.mimeType = std::move(mimeType);
		const auto audio = model.extensions.find("KHR_audio");
		if(audio != model.extensions.end())
		{
			if(!audio->second.IsObject() || !audio->second.Has("sources")
				|| !audio->second.Get("sources").IsArray()
				|| index >= audio->second.Get("sources").ArrayLen()) return false;
			Value& definition = audio->second.Get<Value::Object>()["sources"].Get<Value::Array>()[index];
			if(!definition.IsObject()) return false;
			auto& definitionObject = definition.Get<Value::Object>();
			definitionObject.erase("uri");
			definitionObject["bufferView"] = Value(static_cast<int>(viewIndex));
			definitionObject["mimeType"] = Value(source.mimeType);
		}
	}
	return true;
}

[[nodiscard]] bool unescapePointerToken(std::string_view encoded, std::string& decoded)
{
	decoded.clear();
	decoded.reserve(encoded.size());
	for(std::size_t index = 0; index < encoded.size(); ++index)
	{
		if(encoded[index] != '~') { decoded.push_back(encoded[index]); continue; }
		if(index + 1u >= encoded.size()) return false;
		if(encoded[index + 1u] == '0') decoded.push_back('~');
		else if(encoded[index + 1u] == '1') decoded.push_back('/');
		else return false;
		++index;
	}
	return true;
}

[[nodiscard]] std::string escapePointerToken(std::string_view decoded)
{
	std::string encoded;
	for(const char value : decoded)
	{
		if(value == '~') encoded += "~0";
		else if(value == '/') encoded += "~1";
		else encoded.push_back(value);
	}
	return encoded;
}

[[nodiscard]] bool parsePointerIndex(std::string_view token, std::size_t& index)
{
	if(token.empty() || (token.size() > 1u && token.front() == '0')) return false;
	index = 0;
	for(const char value : token)
	{
		if(value < '0' || value > '9') return false;
		const std::size_t digit = static_cast<std::size_t>(value - '0');
		if(index > (std::numeric_limits<std::size_t>::max() - digit) / 10u) return false;
		index = index * 10u + digit;
	}
	return true;
}

[[nodiscard]] bool remapAnimationPointer(std::string& pointer,
	const Offsets& offsets, const Model& source)
{
	if(pointer.empty() || pointer.front() != '/') return false;
	std::vector<std::string> tokens;
	std::size_t begin = 1u;
	while(begin <= pointer.size())
	{
		const std::size_t end = pointer.find('/', begin);
		const std::string_view encoded(pointer.data() + begin,
			end == std::string::npos ? pointer.size() - begin : end - begin);
		std::string decoded;
		if(!unescapePointerToken(encoded, decoded)) return false;
		tokens.push_back(std::move(decoded));
		if(end == std::string::npos) break;
		begin = end + 1u;
	}
	const auto remapToken = [&](std::size_t tokenIndex,
		std::size_t offset, std::size_t count) -> bool
	{
		if(tokenIndex >= tokens.size()) return false;
		std::size_t value = 0;
		if(!parsePointerIndex(tokens[tokenIndex], value) || value >= count
			|| value > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
		int adjusted = static_cast<int>(value);
		if(!checkedIndexOffset(adjusted, offset)) return false;
		tokens[tokenIndex] = std::to_string(adjusted);
		return true;
	};
	if(!tokens.empty() && tokens[0] == "scenes")
		return false; // Imported scenes are represented by the wrapper node.
	if(tokens.size() >= 2u)
	{
		const auto remapRoot = [&](std::string_view name,
			std::size_t offset, std::size_t count) -> bool
		{
			return tokens[0] != name || remapToken(1u, offset, count);
		};
		if(!remapRoot("nodes", offsets.node, source.nodes.size())
			|| !remapRoot("meshes", offsets.mesh, source.meshes.size())
			|| !remapRoot("materials", offsets.material, source.materials.size())
			|| !remapRoot("textures", offsets.texture, source.textures.size())
			|| !remapRoot("images", offsets.image, source.images.size())
			|| !remapRoot("samplers", offsets.sampler, source.samplers.size())
			|| !remapRoot("accessors", offsets.accessor, source.accessors.size())
			|| !remapRoot("bufferViews", offsets.bufferView, source.bufferViews.size())
			|| !remapRoot("buffers", offsets.buffer, source.buffers.size())
			|| !remapRoot("skins", offsets.skin, source.skins.size())
			|| !remapRoot("cameras", offsets.camera, source.cameras.size())
			|| !remapRoot("animations", offsets.animation, source.animations.size())) return false;
	}
	if(tokens.size() >= 3u && tokens[0] == "extensions")
	{
		std::size_t offset = 0;
		std::size_t count = 0;
		bool knownArray = false;
		if(tokens[1] == "KHR_lights_punctual" && tokens[2] == "lights")
		{
			offset = offsets.light; count = source.lights.size(); knownArray = true;
		}
		else if(tokens[1] == "KHR_audio" && tokens[2] == "emitters")
		{
			offset = offsets.audioEmitter; count = source.audioEmitters.size(); knownArray = true;
		}
		else if(tokens[1] == "KHR_audio" && tokens[2] == "sources")
		{
			offset = offsets.audioSource; count = source.audioSources.size(); knownArray = true;
		}
		else if(tokens[1] == "KHR_materials_variants" && tokens[2] == "variants")
		{
			offset = offsets.variant;
			count = variantCount(source);
			if(count == 0u) return false;
			knownArray = true;
		}
		if(knownArray && !remapToken(3u, offset, count)) return false;
	}
	std::string rebuilt;
	for(const std::string& token : tokens) rebuilt += '/' + escapePointerToken(token);
	pointer = std::move(rebuilt);
	return true;
}

[[nodiscard]] bool remapAnimationPointerExtensions(tinygltf::ExtensionMap& extensions,
	const Offsets& offsets, const Model& source)
{
	const auto found = extensions.find("KHR_animation_pointer");
	if(found == extensions.end()) return true;
	if(!found->second.IsObject() || !found->second.Has("pointer")) return false;
	Value& pointer = found->second.Get<Value::Object>()["pointer"];
	if(!pointer.IsString()) return false;
	std::string text = pointer.Get<std::string>();
	if(!remapAnimationPointer(text, offsets, source)) return false;
	pointer = Value(std::move(text));
	return true;
}

[[nodiscard]] bool remapModelKnownExtensions(Model& candidate,
                                              const Model& source,
                                              const Offsets& offsets)
{
	const auto mergeObjectArrays = [&](std::string_view name,
		const Value& sourceValue,
		std::initializer_list<std::string_view> arrayMembers) -> bool
	{
		if(!sourceValue.IsObject()) return false;
		Value::Object merged;
		const auto target = candidate.extensions.find(std::string(name));
		if(target != candidate.extensions.end())
		{
			if(!target->second.IsObject()) return false;
			merged = target->second.Get<Value::Object>();
		}
		for(const auto& [member, incoming] : sourceValue.Get<Value::Object>())
		{
			const bool append = std::find(arrayMembers.begin(), arrayMembers.end(), member)
				!= arrayMembers.end();
			auto existing = merged.find(member);
			if(!append)
			{
				if(existing == merged.end()) merged.emplace(member, incoming);
				else if(!(existing->second == incoming)) return false;
				continue;
			}
			if(!incoming.IsArray()) return false;
			Value::Array values;
			if(existing != merged.end())
			{
				if(!existing->second.IsArray()) return false;
				values = existing->second.Get<Value::Array>();
			}
			for(const Value& value : incoming.Get<Value::Array>()) values.push_back(value);
			merged[member] = Value(std::move(values));
		}
		candidate.extensions[std::string(name)] = Value(std::move(merged));
		return true;
	};

	for(const auto& [name, member] : {
		std::pair{std::string_view{"KHR_lights_punctual"}, std::string_view{"lights"}},
		std::pair{std::string_view{"KHR_materials_variants"}, std::string_view{"variants"}}})
	{
		const auto extension = source.extensions.find(std::string(name));
		if(extension == source.extensions.end()) continue;
		if(!extension->second.IsObject() || !extension->second.Has(std::string(member))
			|| !extension->second.Get(std::string(member)).IsArray()
			|| !mergeObjectArrays(name, extension->second, {member})) return false;
		if(name == "KHR_lights_punctual"
			&& extension->second.Get(std::string(member)).ArrayLen() != source.lights.size()) return false;
	}

	const auto sourceAudio = source.extensions.find("KHR_audio");
	if(sourceAudio != source.extensions.end())
	{
		if(!sourceAudio->second.IsObject()) return false;
		Value audio(sourceAudio->second);
		auto& object = audio.Get<Value::Object>();
		const auto emitters = object.find("emitters");
		if(emitters == object.end() || !emitters->second.IsArray()
			|| emitters->second.ArrayLen() != source.audioEmitters.size()) return false;
		for(std::size_t index = 0; index < emitters->second.ArrayLen(); ++index)
		{
			Value& emitter = emitters->second.Get<Value::Array>()[index];
			if(!emitter.IsObject() || !emitter.Has("source")
				|| !emitter.Get("source").IsInt()
				|| emitter.Get("source").GetNumberAsInt() != source.audioEmitters[index].source
				|| !offsetIndexValue(emitter.Get<Value::Object>()["source"], offsets.audioSource,
					source.audioSources.size()))
				return false;
		}
		const auto sources = object.find("sources");
		if(sources == object.end() || !sources->second.IsArray()
			|| sources->second.ArrayLen() != source.audioSources.size()) return false;
		for(std::size_t index = 0; index < sources->second.ArrayLen(); ++index)
		{
			Value& audioSource = sources->second.Get<Value::Array>()[index];
			if(!audioSource.IsObject()) return false;
			if(audioSource.Has("bufferView"))
			{
				if(!audioSource.Get("bufferView").IsInt()
					|| audioSource.Get("bufferView").GetNumberAsInt()
						!= source.audioSources[index].bufferView
					|| !offsetIndexValue(
						audioSource.Get<Value::Object>()["bufferView"], offsets.bufferView,
						source.bufferViews.size())) return false;
			}
			else if(source.audioSources[index].bufferView >= 0) return false;
		}
		if(!mergeObjectArrays("KHR_audio", audio, {"emitters", "sources"})) return false;
	}
	return true;
}

[[nodiscard]] bool mergeUnknownTopLevelExtensions(Model& candidate,
	                                               const Model& source)
{
	for(const auto& [name, value] : source.extensions)
	{
		// Known index-bearing extension arrays are handled above. All
		// other top-level extension values can be copied without interpretation.
		if(name == "KHR_lights_punctual" || name == "KHR_materials_variants"
			|| name == "KHR_audio") continue;
		auto existing = candidate.extensions.find(name);
		if(existing == candidate.extensions.end())
		{
			candidate.extensions.emplace(name, value);
		}
		else if(!(existing->second == value))
		{
			// There is no lossless representation for two different values under
			// one glTF extension key; reject the candidate instead of dropping one.
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool mergeModels(Model& candidate, const Model& source,
                             std::size_t targetSceneIndex,
                             const std::string& wrapperName,
                             std::size_t& wrapperIndex,
                             const Offsets& offsets)
{
	if(source.scenes.empty() || candidate.scenes.empty()
		|| targetSceneIndex >= candidate.scenes.size()) return false;
	const int sourceSceneIndex = source.defaultScene >= 0
		&& static_cast<std::size_t>(source.defaultScene) < source.scenes.size()
		? source.defaultScene : 0;
	const tinygltf::Scene& sourceScene = source.scenes[static_cast<std::size_t>(sourceSceneIndex)];
	if(source.nodes.empty()) return false;

	for(const auto& value : source.buffers)
	{
		tinygltf::Buffer copy = value;
		// TinyGLTF has already decoded the external/data URI into `data`. Clear
		// incoming URIs so Save As emits one self-contained resource set and does
		// not accidentally resolve paths relative to the wrong source document.
		if(copy.data.empty() && !copy.uri.empty()) return false;
		copy.uri.clear();
		candidate.buffers.push_back(std::move(copy));
	}
	for(const auto& value : source.bufferViews)
	{
		tinygltf::BufferView copy = value;
		if(!checkedIndexOffset(copy.buffer, offsets.buffer)) return false;
		if(auto extension = copy.extensions.find("EXT_meshopt_compression");
			extension != copy.extensions.end())
		{
			if(!extension->second.IsObject()) return false;
			auto& object = extension->second.Get<Value::Object>();
			bool found = false;
			for(auto& member : object)
			{
				if(member.first != "buffer") continue;
				found = true;
				if(!offsetIndexValue(member.second, offsets.buffer, source.buffers.size())) return false;
			}
			if(!found) return false;
		}
		candidate.bufferViews.push_back(std::move(copy));
	}
	for(const auto& value : source.accessors)
	{
		tinygltf::Accessor copy = value;
		if(!checkedIndexOffset(copy.bufferView, offsets.bufferView)
			|| !checkedIndexOffset(copy.sparse.indices.bufferView, offsets.bufferView)
			|| !checkedIndexOffset(copy.sparse.values.bufferView, offsets.bufferView))
			return false;
		candidate.accessors.push_back(std::move(copy));
	}
	for(const auto& value : source.images)
	{
		tinygltf::Image copy = value;
		if(copy.bufferView >= 0 && !checkedIndexOffset(copy.bufferView, offsets.bufferView)) return false;
		if(copy.image.empty() && copy.bufferView < 0 && !copy.uri.empty()) return false;
		copy.uri.clear();
		candidate.images.push_back(std::move(copy));
	}
	for(const auto& value : source.samplers) candidate.samplers.push_back(value);
	for(const auto& value : source.textures)
	{
		tinygltf::Texture copy = value;
		if(!remapIndex(copy.source, offsets.image, source.images.size())
			|| !remapIndex(copy.sampler, offsets.sampler, source.samplers.size())
			|| !remapKnownTextureExtensions(copy, offsets, source.images.size())) return false;
		candidate.textures.push_back(std::move(copy));
	}
	for(const auto& value : source.materials)
	{
		tinygltf::Material copy = value;
		if(!remapMaterial(copy, offsets, source.textures.size(), source.materials.size())) return false;
		candidate.materials.push_back(std::move(copy));
	}
	for(const auto& value : source.meshes)
	{
		tinygltf::Mesh copy = value;
		for(auto& primitive : copy.primitives)
			if(!remapPrimitive(primitive, offsets, source)) return false;
		candidate.meshes.push_back(std::move(copy));
	}
	for(const auto& value : source.skins)
	{
		tinygltf::Skin copy = value;
		if(!checkedIndexOffset(copy.inverseBindMatrices, offsets.accessor)
			|| !checkedIndexOffset(copy.skeleton, offsets.node)) return false;
		for(int& joint : copy.joints)
			if(!checkedIndexOffset(joint, offsets.node)) return false;
		candidate.skins.push_back(std::move(copy));
	}
	for(const auto& value : source.cameras) candidate.cameras.push_back(value);
	for(const auto& value : source.nodes)
	{
		tinygltf::Node copy = value;
		if(!remapNode(copy, offsets, source)) return false;
		candidate.nodes.push_back(std::move(copy));
	}
	for(const auto& value : source.animations)
	{
		tinygltf::Animation copy = value;
		for(auto& channel : copy.channels)
		{
			if(!checkedIndexOffset(channel.target_node, offsets.node)
				|| !remapAnimationPointerExtensions(channel.extensions, offsets, source)
				|| !remapAnimationPointerExtensions(channel.target_extensions, offsets, source)) return false;
		}
		for(auto& sampler : copy.samplers)
			if(!checkedIndexOffset(sampler.input, offsets.accessor)
				|| !checkedIndexOffset(sampler.output, offsets.accessor)) return false;
		candidate.animations.push_back(std::move(copy));
	}
	for(const auto& value : source.lights) candidate.lights.push_back(value);
	for(const auto& value : source.audioSources)
	{
		tinygltf::AudioSource copy = value;
		if(copy.bufferView >= 0 && !checkedIndexOffset(copy.bufferView, offsets.bufferView)) return false;
		if(copy.bufferView >= 0) copy.uri.clear();
		candidate.audioSources.push_back(std::move(copy));
	}
	for(const auto& value : source.audioEmitters)
	{
		tinygltf::AudioEmitter copy = value;
		if(!checkedIndexOffset(copy.source, offsets.audioSource)) return false;
		candidate.audioEmitters.push_back(std::move(copy));
	}

	tinygltf::Node wrapper;
	wrapper.name = wrapperName;
	// Keep imported scene metadata attached to the wrapper occurrence. The
	// source scene itself is not appended as a second document scene.
	tinygltf::Scene sceneMetadata = sourceScene;
	if(!remapSceneExtensions(sceneMetadata, offsets, source)) return false;
	if(const auto importedAudio = sceneMetadata.extensions.find("KHR_audio");
		importedAudio != sceneMetadata.extensions.end())
	{
		if(!importedAudio->second.IsObject()) return false;
		Value::Object merged;
		auto targetAudio = candidate.scenes[targetSceneIndex].extensions.find("KHR_audio");
		if(targetAudio != candidate.scenes[targetSceneIndex].extensions.end())
		{
			if(!targetAudio->second.IsObject()) return false;
			merged = targetAudio->second.Get<Value::Object>();
		}
		for(const auto& [member, value] : importedAudio->second.Get<Value::Object>())
		{
			auto existing = merged.find(member);
			if(member != "emitters")
			{
				if(existing == merged.end()) merged.emplace(member, value);
				else if(!(existing->second == value)) return false;
				continue;
			}
			if(!value.IsArray()) return false;
			Value::Array emitters;
			if(existing != merged.end())
			{
				if(!existing->second.IsArray()) return false;
				emitters = existing->second.Get<Value::Array>();
			}
			for(const Value& emitter : value.Get<Value::Array>()) emitters.push_back(emitter);
			merged[member] = Value(std::move(emitters));
		}
		candidate.scenes[targetSceneIndex].extensions["KHR_audio"] = Value(std::move(merged));
		sceneMetadata.extensions.erase("KHR_audio");
	}
	wrapper.extensions = sceneMetadata.extensions;
	wrapper.extras = sceneMetadata.extras;
	wrapper.extensions_json_string = sceneMetadata.extensions_json_string;
	wrapper.extras_json_string = sceneMetadata.extras_json_string;
	for(const int root : sourceScene.nodes)
	{
		int remapped = root;
		if(!checkedIndexOffset(remapped, offsets.node)) return false;
		wrapper.children.push_back(remapped);
	}
	wrapperIndex = candidate.nodes.size();
	if(wrapperIndex > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
	candidate.nodes.push_back(std::move(wrapper));
	candidate.scenes[targetSceneIndex].nodes.push_back(
		static_cast<int>(wrapperIndex));
	for(const int emitter : sourceScene.audioEmitters)
	{
		int remapped = emitter;
		if(!checkedIndexOffset(remapped, offsets.audioEmitter)) return false;
		candidate.scenes[targetSceneIndex].audioEmitters.push_back(remapped);
	}

	for(const std::string& extension : source.extensionsUsed)
		if(std::find(candidate.extensionsUsed.begin(), candidate.extensionsUsed.end(), extension)
			== candidate.extensionsUsed.end()) candidate.extensionsUsed.push_back(extension);
	for(const std::string& extension : source.extensionsRequired)
		if(std::find(candidate.extensionsRequired.begin(), candidate.extensionsRequired.end(), extension)
			== candidate.extensionsRequired.end()) candidate.extensionsRequired.push_back(extension);
	return remapModelKnownExtensions(candidate, source, offsets)
		&& mergeUnknownTopLevelExtensions(candidate, source);
}

[[nodiscard]] std::size_t primitiveCount(const Model& model) noexcept
{
	std::size_t count = 0;
	for(const auto& mesh : model.meshes)
	{
		if(mesh.primitives.size() > std::numeric_limits<std::size_t>::max() - count)
			return std::numeric_limits<std::size_t>::max();
		count += mesh.primitives.size();
	}
	return count;
}

[[nodiscard]] SceneMergeStatus translateCommit(detail::MutationCommitStatus status) noexcept
{
	switch(status)
	{
	case detail::MutationCommitStatus::applied: return SceneMergeStatus::applied;
	case detail::MutationCommitStatus::allocationFailed: return SceneMergeStatus::allocationFailed;
	case detail::MutationCommitStatus::generationExhausted: return SceneMergeStatus::generationExhausted;
	case detail::MutationCommitStatus::revisionExhausted: return SceneMergeStatus::revisionExhausted;
	case detail::MutationCommitStatus::prePublishFailed: return SceneMergeStatus::injectedFailure;
	default: return SceneMergeStatus::internalError;
	}
}

struct PublishContext final
{
	EditorFaultInjector* injector{nullptr};
};

void prePublish(const detail::MutationRegistryPlan&, void* context)
{
	auto* publish = static_cast<PublishContext*>(context);
	if(publish != nullptr && publish->injector != nullptr
		&& publish->injector->shouldFail(EditorFaultPoint::preCandidatePublish))
		throw std::runtime_error("merge publication fault injected");
}

void setMessage(SceneMergeResult& result, std::string_view message) noexcept
{
	try { result.message.assign(message); } catch(...) { result.message.clear(); }
}

} // namespace

SceneMergeResult SceneMerger::merge(GltfDocument& target,
	const GltfDocument& incoming,
	std::string_view wrapperName,
	const EditorLimits& limits,
	EditorFaultInjector* faultInjector) noexcept
{
	SceneMergeResult result;
	result.beforeRevision = target.revision();
	result.afterRevision = result.beforeRevision;
	try
	{
		const Model& targetModel = detail::SceneMergerAccess::model(target);
		const Model& sourceAuthority = detail::SceneMergerAccess::model(incoming);
		const GltfValidationReport targetReport = GltfValidator::validate(target, limits);
		if(!targetReport.valid()) { result.status = SceneMergeStatus::invalidTarget; setMessage(result, "target document failed validation"); return result; }
		auto preparedSource = std::make_unique<Model>(sourceAuthority);
		if(!stageExternalAudio(*preparedSource, incoming, limits))
		{
			result.status = SceneMergeStatus::invalidIncoming;
			setMessage(result, "incoming audio resource could not be staged safely");
			return result;
		}
		const GltfValidationReport sourceReport =
			detail::SceneMergerAccess::validateCandidate(preparedSource, limits);
		if(!sourceReport.valid()) { result.status = SceneMergeStatus::invalidIncoming; setMessage(result, "incoming document failed validation"); return result; }
		const Model& sourceModel = *preparedSource;
		if(sourceModel.scenes.empty() || sourceModel.nodes.empty()) { result.status = SceneMergeStatus::invalidIncoming; setMessage(result, "incoming document has no scene roots"); return result; }
		if(targetModel.scenes.empty() || !target.currentSceneIndex()) { result.status = SceneMergeStatus::invalidTarget; setMessage(result, "target document has no active scene"); return result; }
		if(faultInjector != nullptr && faultInjector->shouldFail(EditorFaultPoint::allocation)) { result.status = SceneMergeStatus::injectedFailure; setMessage(result, "merge allocation fault injected"); return result; }

		const auto fits = [&](std::size_t base, std::size_t add, std::size_t limit) noexcept
		{
			std::size_t total = 0;
			return appendSize(base, add, total) && total <= limit;
		};
		std::size_t incomingNodesWithWrapper = 0;
		const std::size_t maxIndexedCollection =
			static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1u;
		const auto indexedLimit = [maxIndexedCollection](std::size_t configured) noexcept
		{
			return std::min(configured, maxIndexedCollection);
		};
		const std::size_t targetVariants = variantCount(targetModel);
		const std::size_t sourceVariants = variantCount(sourceModel);
		if(!appendSize(sourceModel.nodes.size(), 1u, incomingNodesWithWrapper)
			|| !fits(targetModel.nodes.size(), incomingNodesWithWrapper,
				indexedLimit(limits.maxNodes))
			|| !fits(targetModel.meshes.size(), sourceModel.meshes.size(), indexedLimit(limits.maxMeshes))
			|| !fits(targetModel.materials.size(), sourceModel.materials.size(), indexedLimit(limits.maxMaterials))
			|| !fits(targetModel.textures.size(), sourceModel.textures.size(), indexedLimit(limits.maxTextures))
			|| !fits(targetModel.images.size(), sourceModel.images.size(), indexedLimit(limits.maxImages))
			|| !fits(targetModel.samplers.size(), sourceModel.samplers.size(), indexedLimit(limits.maxSamplers))
			|| !fits(targetModel.accessors.size(), sourceModel.accessors.size(), indexedLimit(limits.maxAccessors))
			|| !fits(targetModel.bufferViews.size(), sourceModel.bufferViews.size(), indexedLimit(limits.maxBufferViews))
			|| !fits(targetModel.buffers.size(), sourceModel.buffers.size(), indexedLimit(limits.maxBuffers))
			|| !fits(targetModel.skins.size(), sourceModel.skins.size(), indexedLimit(limits.maxSkins))
			|| !fits(targetModel.animations.size(), sourceModel.animations.size(), indexedLimit(limits.maxAnimations))
			|| !fits(targetModel.cameras.size(), sourceModel.cameras.size(), indexedLimit(limits.maxCameras))
			|| !fits(targetModel.lights.size(), sourceModel.lights.size(), indexedLimit(limits.maxLights))
			|| !fits(targetModel.audioEmitters.size(), sourceModel.audioEmitters.size(), indexedLimit(limits.maxAudioEmitters))
			|| !fits(targetModel.audioSources.size(), sourceModel.audioSources.size(), indexedLimit(limits.maxAudioSources))
			|| !fits(targetVariants, sourceVariants, maxIndexedCollection)
			|| !fits(primitiveCount(targetModel), primitiveCount(sourceModel), limits.maxPrimitives))
		{
			result.status = SceneMergeStatus::limitExceeded;
			setMessage(result, "merge exceeds configured editor limits");
			return result;
		}

		Offsets offsets{
			targetModel.nodes.size(), targetModel.meshes.size(), targetModel.materials.size(),
			targetModel.textures.size(), targetModel.images.size(), targetModel.samplers.size(),
			targetModel.accessors.size(), targetModel.bufferViews.size(), targetModel.buffers.size(),
			targetModel.skins.size(), targetModel.cameras.size(), targetModel.animations.size(),
			targetModel.lights.size(), targetModel.audioEmitters.size(), targetModel.audioSources.size(),
			targetVariants};

		auto candidate = std::make_unique<Model>(targetModel);
		std::size_t wrapperIndex = 0;
		std::string name(wrapperName);
		if(name.empty()) name = sourceModel.scenes[sourceModel.defaultScene >= 0 ? sourceModel.defaultScene : 0].name;
		if(name.empty()) name = incoming.sourcePath().stem().string();
		if(name.empty()) name = "Imported Scene";
		if(!mergeModels(*candidate, sourceModel, *target.currentSceneIndex(), name, wrapperIndex, offsets))
		{
			result.status = SceneMergeStatus::unsupportedExtensionReference;
			setMessage(result, "incoming resource or known extension reference cannot be remapped");
			return result;
		}

		const GltfValidationReport candidateReport = detail::SceneMergerAccess::validateCandidate(candidate, limits);
		if(!candidateReport.valid())
		{
			result.status = SceneMergeStatus::validationFailed;
			setMessage(result, "merge candidate failed validation");
			return result;
		}
		if(faultInjector != nullptr && faultInjector->shouldFail(EditorFaultPoint::preCandidatePublish))
		{
			result.status = SceneMergeStatus::injectedFailure;
			setMessage(result, "merge candidate publication fault injected");
			return result;
		}

		detail::MutationRegistryPlan plan;
		const auto preserve = [&](EditorObjectKind kind, std::size_t oldCount, std::size_t newCount)
		{
			auto& ids = plan.idsByKind[static_cast<std::size_t>(kind)];
			ids.reserve(newCount);
			for(std::size_t index = 0; index < oldCount; ++index) ids.push_back(target.idAt(kind, index));
			while(ids.size() < newCount) ids.push_back(EditorObjectId{});
		};
		preserve(EditorObjectKind::scene, targetModel.scenes.size(), candidate->scenes.size());
		preserve(EditorObjectKind::node, targetModel.nodes.size(), candidate->nodes.size());
		preserve(EditorObjectKind::primitive, primitiveCount(targetModel), primitiveCount(*candidate));
		preserve(EditorObjectKind::mesh, targetModel.meshes.size(), candidate->meshes.size());
		preserve(EditorObjectKind::material, targetModel.materials.size(), candidate->materials.size());
		preserve(EditorObjectKind::texture, targetModel.textures.size(), candidate->textures.size());
		preserve(EditorObjectKind::image, targetModel.images.size(), candidate->images.size());
		preserve(EditorObjectKind::sampler, targetModel.samplers.size(), candidate->samplers.size());
		preserve(EditorObjectKind::camera, targetModel.cameras.size(), candidate->cameras.size());
		preserve(EditorObjectKind::light, targetModel.lights.size(), candidate->lights.size());
		preserve(EditorObjectKind::accessor, targetModel.accessors.size(), candidate->accessors.size());
		preserve(EditorObjectKind::bufferView, targetModel.bufferViews.size(), candidate->bufferViews.size());
		preserve(EditorObjectKind::buffer, targetModel.buffers.size(), candidate->buffers.size());
		preserve(EditorObjectKind::skin, targetModel.skins.size(), candidate->skins.size());
		preserve(EditorObjectKind::animation, targetModel.animations.size(), candidate->animations.size());
		plan.primitiveAddresses.reserve(primitiveCount(*candidate));
		for(std::size_t mesh = 0; mesh < candidate->meshes.size(); ++mesh)
			for(std::size_t primitive = 0; primitive < candidate->meshes[mesh].primitives.size(); ++primitive)
			{
				if(mesh > std::numeric_limits<std::uint32_t>::max()
					|| primitive > std::numeric_limits<std::uint32_t>::max())
				{
					result.status = SceneMergeStatus::limitExceeded;
					setMessage(result, "merge primitive address exceeds editor index space");
					return result;
				}
				plan.primitiveAddresses.push_back(PrimitiveAddress{static_cast<std::uint32_t>(mesh), static_cast<std::uint32_t>(primitive)});
			}
		plan.currentScene = target.currentSceneId();
		plan.hierarchyChanged = true;
		PublishContext context{faultInjector};
		const detail::MutationCommitStatus commitStatus = detail::SceneMergerAccess::commit(
			target, std::move(candidate), plan, prePublish, &context);
		result.status = translateCommit(commitStatus);
		if(result.status != SceneMergeStatus::applied)
		{
			setMessage(result, "merge candidate could not be published atomically");
			return result;
		}
		result.afterRevision = target.revision();
		result.wrapperNode = NodeId::from(target.idAt(EditorObjectKind::node, wrapperIndex));
		setMessage(result, "scene merged below wrapper root");
		return result;
	}
	catch(const std::bad_alloc&)
	{
		result.status = SceneMergeStatus::allocationFailed;
		setMessage(result, "merge allocation failed before publication");
	}
	catch(...)
	{
		result.status = SceneMergeStatus::internalError;
		setMessage(result, "merge failed before atomic publication");
	}
	return result;
}

} // namespace demo::editor
