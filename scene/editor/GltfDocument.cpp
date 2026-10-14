#include <mgif/scene_editor/GltfDocument.h>
#include <mgif/scene_editor/SceneChangeCanonicalizer.h>
#include <mgif/scene_editor/testing/EditorStateInspector.h>

#include <tiny_gltf.h>
#include "../../loader/GltfCanonicalWriter.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace demo::editor {
namespace {

constexpr std::size_t kKindCount = static_cast<std::size_t>(EditorObjectKind::count);

template<typename T>
[[nodiscard]] bool checkedCounterSuccessor(T current, T& next) noexcept
{
	if(current == std::numeric_limits<T>::max())
	{
		next = current;
		return false;
	}
	next = static_cast<T>(current + 1u);
	return true;
}

// Zero permanently marks exhaustion. Authorities are never recycled, even if
// the document construction that reserved one later fails.
constinit std::atomic<std::uint64_t> g_nextDocumentAuthority{1u};

[[nodiscard]] EditorDocumentAuthority allocateDocumentAuthority()
{
	std::uint64_t candidate = g_nextDocumentAuthority.load(std::memory_order_relaxed);
	while(candidate != 0)
	{
		const std::uint64_t successor = candidate == std::numeric_limits<std::uint64_t>::max()
			? 0
			: candidate + 1u;
		if(g_nextDocumentAuthority.compare_exchange_weak(
			candidate,
			successor,
			std::memory_order_relaxed,
			std::memory_order_relaxed))
		{
			return EditorDocumentAuthority{candidate};
		}
	}

	throw std::overflow_error("editor document authority space is exhausted");
}

[[nodiscard]] constexpr std::size_t kindIndex(EditorObjectKind kind) noexcept
{
	return static_cast<std::size_t>(kind);
}

[[nodiscard]] bool isObjectKind(EditorObjectKind kind) noexcept
{
	return kind > EditorObjectKind::invalid && kind < EditorObjectKind::count;
}

[[nodiscard]] std::size_t modelObjectCount(
	const tinygltf::Model& model, EditorObjectKind kind) noexcept
{
	switch(kind)
	{
		case EditorObjectKind::scene: return model.scenes.size();
		case EditorObjectKind::node: return model.nodes.size();
		case EditorObjectKind::primitive:
		{
			std::size_t count = 0;
			for(const auto& mesh : model.meshes) count += mesh.primitives.size();
			return count;
		}
		case EditorObjectKind::mesh: return model.meshes.size();
		case EditorObjectKind::material: return model.materials.size();
		case EditorObjectKind::texture: return model.textures.size();
		case EditorObjectKind::image: return model.images.size();
		case EditorObjectKind::sampler: return model.samplers.size();
		case EditorObjectKind::camera: return model.cameras.size();
		case EditorObjectKind::light: return model.lights.size();
		case EditorObjectKind::accessor: return model.accessors.size();
		case EditorObjectKind::bufferView: return model.bufferViews.size();
		case EditorObjectKind::buffer: return model.buffers.size();
		case EditorObjectKind::skin: return model.skins.size();
		case EditorObjectKind::animation: return model.animations.size();
		case EditorObjectKind::invalid:
		case EditorObjectKind::count: return 0;
	}
	return 0;
}

[[nodiscard]] std::string lowerAscii(std::string value)
{
	for(char& character : value)
	{
		character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	}
	return value;
}

[[nodiscard]] std::string canonicalImageExtension(const tinygltf::Image& image)
{
	const std::string mime = lowerAscii(image.mimeType);
	if(mime == "image/jpeg") return "jpg";
	if(mime == "image/png") return "png";
	if(mime == "image/bmp") return "bmp";
	if(mime == "image/gif") return "gif";
	if(mime == "image/webp") return "webp";
	if(mime == "image/ktx2") return "ktx2";

	std::string extension = lowerAscii(std::filesystem::path(image.uri).extension().string());
	if(!extension.empty() && extension.front() == '.')
	{
		extension.erase(extension.begin());
	}
	for(const char character : extension)
	{
		if(!std::isalnum(static_cast<unsigned char>(character)))
		{
			return "bin";
		}
	}
	return extension.empty() ? "bin" : extension;
}

[[nodiscard]] std::string canonicalImageMime(const tinygltf::Image& image)
{
	if(!image.mimeType.empty())
	{
		return lowerAscii(image.mimeType);
	}
	const std::string extension = canonicalImageExtension(image);
	if(extension == "jpg" || extension == "jpeg") return "image/jpeg";
	if(extension == "png") return "image/png";
	if(extension == "bmp") return "image/bmp";
	if(extension == "gif") return "image/gif";
	if(extension == "webp") return "image/webp";
	if(extension == "ktx2") return "image/ktx2";
	return "application/octet-stream";
}

[[nodiscard]] std::string encodeBase64(const std::vector<unsigned char>& bytes)
{
	return SceneChangeCanonicalizer::encodeBase64(bytes);
}

using CanonicalImageUris = std::vector<std::shared_ptr<const std::string>>;

[[nodiscard]] std::shared_ptr<const std::string> canonicalPayloadUri(
	const std::vector<unsigned char>& payload, const std::string& mime)
{
	struct Entry
	{
		std::vector<unsigned char> bytes;
		std::string mime;
		std::shared_ptr<const std::string> uri;
		std::size_t retainedBytes() const noexcept
		{
			return bytes.capacity() + mime.capacity() + uri->capacity();
		}
	};
	struct Cache
	{
		std::mutex mutex;
		std::vector<Entry> entries;
		std::size_t retainedBytes{0};
	};
	static Cache cache;
	constexpr std::size_t budget = 512u * 1024u * 1024u;
	const auto find = [&]() -> std::shared_ptr<const std::string>
	{
		for(const auto& entry : cache.entries)
		{
			if(entry.mime == mime && entry.bytes.size() == payload.size() &&
				(payload.empty() || std::memcmp(entry.bytes.data(), payload.data(), payload.size()) == 0))
				return entry.uri;
		}
		return {};
	};
	{
		const std::lock_guard lock(cache.mutex);
		if(auto cached = find()) return cached;
	}
	const auto encoded = encodeBase64(payload);
	auto uri = std::make_shared<std::string>();
	uri->reserve(5u + mime.size() + 8u + encoded.size());
	uri->append("data:").append(mime).append(";base64,").append(encoded);
	if(payload.size() + uri->capacity() + mime.size() > budget) return uri;
	{
		const std::lock_guard lock(cache.mutex);
		if(auto cached = find()) return cached;
		Entry entry{payload, mime, uri};
		const auto bytes = entry.retainedBytes();
		while(!cache.entries.empty() &&
			(cache.retainedBytes + bytes > budget || cache.entries.size() >= 128u))
		{
			cache.retainedBytes -= cache.entries.front().retainedBytes();
			cache.entries.erase(cache.entries.begin());
		}
		if(bytes <= budget)
		{
			cache.entries.push_back(std::move(entry));
			cache.retainedBytes += bytes;
		}
	}
	return uri;
}

[[nodiscard]] std::string canonicalImagePlaceholder(std::size_t index)
{
	return "__MGIF_CANONICAL_IMAGE_URI_" + std::to_string(index) + "__";
}

[[nodiscard]] bool writeCanonicalImage(
	const std::string*,
	const std::string*,
	const tinygltf::Image* image,
	bool,
	const tinygltf::FsCallbacks*,
	const tinygltf::URICallbacks*,
	std::string* outputUri,
	void* userData)
{
	if(image == nullptr || outputUri == nullptr)
	{
		return false;
	}
	const std::string mime = canonicalImageMime(*image);
	const auto uri = canonicalPayloadUri(image->image, mime);
	// Base64 is already JSON-safe ASCII. Keep large payloads out of the JSON
	// serializer's per-character escaping loop, then splice the exact bytes in.
	if(userData != nullptr && std::all_of(mime.begin(), mime.end(),
		[](unsigned char c) { return c >= 0x20 && c < 0x7f && c != '"' && c != '\\'; }))
	{
		auto& uris = *static_cast<CanonicalImageUris*>(userData);
		const auto placeholder = canonicalImagePlaceholder(uris.size());
		uris.push_back(uri);
		*outputUri = placeholder;
	}
	else
	{
		*outputUri = *uri;
	}
	return true;
}

[[nodiscard]] std::string canonicalMetadata(const tinygltf::Model &model, CanonicalImageUris &uris,
											bool reusePayloads,
											detail::CanonicalGltfMaterialMetadata *materialMetadata)
{
	std::vector<std::string> bufferUris;
	std::vector<std::string> imageUris;
	std::size_t uriIndex = 0u;
	const auto payloadUri = [&](const auto &bytes, const std::string &mime)
	{
		if(!reusePayloads)
			uris.push_back(canonicalPayloadUri(bytes, mime));
		if(uriIndex >= uris.size())
			throw std::runtime_error("canonical payload cache is incomplete");
		return canonicalImagePlaceholder(uriIndex++);
	};
	for(const auto &buffer : model.buffers)
		bufferUris.push_back(payloadUri(buffer.data, "application/octet-stream"));
	for(const auto &image : model.images)
	{
		if(image.uri.empty() && image.bufferView >= 0)
		{
			imageUris.emplace_back();
			continue;
		}
		const std::string mime = canonicalImageMime(image);
		if(std::all_of(mime.begin(), mime.end(),
					   [](unsigned char c) { return c >= 0x20 && c < 0x7f && c != '"' && c != '\\'; }))
			imageUris.push_back(payloadUri(image.image, mime));
		else
			imageUris.push_back(*canonicalPayloadUri(image.image, mime));
	}
	if(uriIndex != uris.size())
		throw std::runtime_error("canonical payload cache differs from authority");
	std::ostringstream stream(std::ios::out | std::ios::binary);
	if(!detail::writeCanonicalGltfMetadata(model, stream, bufferUris, imageUris, materialMetadata) ||
	   !stream.good())
		throw std::runtime_error("TinyGLTF could not serialize the canonical document representation");
	return stream.str();
}

struct CanonicalUriReplacement final
{
	std::size_t position, length;
	const std::string *uri;
};

[[nodiscard]] std::optional<std::vector<CanonicalUriReplacement>>
canonicalUriReplacements(const std::string &json, const CanonicalImageUris &uris)
{
	std::vector<CanonicalUriReplacement> replacements;
	for(std::size_t index = 0; index < uris.size(); ++index)
	{
		const auto token = '"' + canonicalImagePlaceholder(index) + '"';
		const auto position = json.find(token);
		// A user extension/name may contain our temporary token. Fall back to
		// ordinary serialization in that case so no user-owned text is replaced.
		if(position == std::string::npos || json.find(token, position + token.size()) != std::string::npos)
			return std::nullopt;
		replacements.push_back({position, token.size(), uris[index].get()});
	}
	std::sort(replacements.begin(), replacements.end(),
			  [](const auto &left, const auto &right) { return left.position < right.position; });
	return replacements;
}

[[nodiscard]] std::uint64_t appendSemanticHash(std::uint64_t hash,
											   std::span<const std::uint8_t> bytes) noexcept
{
	if(bytes.empty())
		return hash;
	const auto *cursor = bytes.data();
	const auto *end = cursor + bytes.size();
	while(cursor != end)
	{
		hash ^= *cursor++;
		hash *= 1099511628211ull;
	}
	return hash;
}

[[nodiscard]] std::string semanticHashHex(std::uint64_t hash)
{
	static constexpr char digits[] = "0123456789abcdef";
	std::string result(16u, '0');
	for(std::size_t index = 0; index < result.size(); ++index)
		result[index] = digits[(hash >> ((result.size() - index - 1u) * 4u)) & 0x0fu];
	return result;
}

[[nodiscard]] bool expandCanonicalImageUris(std::string &json, const CanonicalImageUris &uris)
{
	const auto replacements = canonicalUriReplacements(json, uris);
	if(!replacements)
		return false;
	std::size_t outputSize = json.size();
	for(const auto &replacement : *replacements)
	{
		const auto *uri = replacement.uri;
		outputSize -= replacement.length;
		if(outputSize > json.max_size() - 2u || uri->size() > json.max_size() - outputSize - 2u)
			return false;
		outputSize += uri->size() + 2u;
	}
	if(replacements->empty())
		return true;
	std::string expanded;
	expanded.reserve(outputSize);
	std::size_t offset = 0;
	for(const auto &replacement : *replacements)
	{
		expanded.append(json, offset, replacement.position - offset);
		expanded.push_back('"');
		expanded.append(*replacement.uri);
		expanded.push_back('"');
		offset = replacement.position + replacement.length;
	}
	expanded.append(json, offset, std::string::npos);
	json.swap(expanded);
	return true;
}

[[nodiscard]] constexpr std::uint32_t rotateRight(
	std::uint32_t value, unsigned bits) noexcept
{
	return (value >> bits) | (value << (32u - bits));
}

[[nodiscard]] std::string sha256Hex(std::string_view bytes)
{
	static constexpr std::array<std::uint32_t, 64> constants{
		0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
		0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
		0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
		0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
		0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
		0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
		0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
		0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
		0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
		0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
		0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
		0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
		0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
		0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
		0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
		0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
	};
	std::vector<std::uint8_t> padded(bytes.begin(), bytes.end());
	const std::uint64_t bitCount = static_cast<std::uint64_t>(padded.size()) * 8u;
	padded.push_back(0x80u);
	while((padded.size() % 64u) != 56u) padded.push_back(0u);
	for(int shift = 56; shift >= 0; shift -= 8)
		padded.push_back(static_cast<std::uint8_t>((bitCount >> shift) & 0xffu));

	std::array<std::uint32_t, 8> hash{
		0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
		0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
	for(std::size_t offset = 0; offset < padded.size(); offset += 64u)
	{
		std::array<std::uint32_t, 64> words{};
		for(std::size_t index = 0; index < 16u; ++index)
		{
			const std::size_t byte = offset + index * 4u;
			words[index] = (static_cast<std::uint32_t>(padded[byte]) << 24u)
				| (static_cast<std::uint32_t>(padded[byte + 1u]) << 16u)
				| (static_cast<std::uint32_t>(padded[byte + 2u]) << 8u)
				| static_cast<std::uint32_t>(padded[byte + 3u]);
		}
		for(std::size_t index = 16u; index < words.size(); ++index)
		{
			const std::uint32_t s0 = rotateRight(words[index - 15u], 7u)
				^ rotateRight(words[index - 15u], 18u) ^ (words[index - 15u] >> 3u);
			const std::uint32_t s1 = rotateRight(words[index - 2u], 17u)
				^ rotateRight(words[index - 2u], 19u) ^ (words[index - 2u] >> 10u);
			words[index] = words[index - 16u] + s0 + words[index - 7u] + s1;
		}
		std::array<std::uint32_t, 8> work = hash;
		for(std::size_t index = 0; index < words.size(); ++index)
		{
			const std::uint32_t s1 = rotateRight(work[4], 6u)
				^ rotateRight(work[4], 11u) ^ rotateRight(work[4], 25u);
			const std::uint32_t choose = (work[4] & work[5]) ^ (~work[4] & work[6]);
			const std::uint32_t temporary1 = work[7] + s1 + choose
				+ constants[index] + words[index];
			const std::uint32_t s0 = rotateRight(work[0], 2u)
				^ rotateRight(work[0], 13u) ^ rotateRight(work[0], 22u);
			const std::uint32_t majority = (work[0] & work[1])
				^ (work[0] & work[2]) ^ (work[1] & work[2]);
			const std::uint32_t temporary2 = s0 + majority;
			work[7] = work[6]; work[6] = work[5]; work[5] = work[4];
			work[4] = work[3] + temporary1; work[3] = work[2];
			work[2] = work[1]; work[1] = work[0]; work[0] = temporary1 + temporary2;
		}
		for(std::size_t index = 0; index < hash.size(); ++index) hash[index] += work[index];
	}
	static constexpr char hex[] = "0123456789abcdef";
	std::string result;
	result.reserve(64u);
	for(const std::uint32_t value : hash)
		for(int shift = 28; shift >= 0; shift -= 4)
			result.push_back(hex[(value >> shift) & 0xfu]);
	return result;
}

template <typename T>
void applyPermutation(std::vector<T>& values, const std::vector<std::uint32_t>& oldToNew)
{
	std::vector<T> reordered(values.size());
	for(std::size_t old = 0; old < values.size(); ++old)
		reordered[oldToNew[old]] = std::move(values[old]);
	values.swap(reordered);
}

struct ReferenceRewriter final
{
	tinygltf::Model& model;
	EditorObjectKind targetKind;
	const std::vector<std::uint32_t>& oldToNew;
	ReferenceRewriteProbeResult& result;
	bool valid{true};

	void index(int& value, std::string path, bool optional = true)
	{
		if(value < 0)
		{
			if(!optional) valid = false;
			return;
		}
		ReferenceLocationInspection location;
		location.jsonPointer = std::move(path);
		location.before = value;
		if(static_cast<std::size_t>(value) >= oldToNew.size())
		{
			valid = false;
			result.locations.push_back(std::move(location));
			return;
		}
		value = static_cast<int>(oldToNew[static_cast<std::size_t>(value)]);
		location.after = value;
		location.rewritten = location.before != value;
		result.locations.push_back(std::move(location));
	}

	void valueIndex(tinygltf::Value& value, std::string path, bool codecLocal = false)
	{
		ReferenceLocationInspection location;
		location.jsonPointer = std::move(path);
		location.codecLocal = codecLocal;
		if(!value.IsInt() || value.GetNumberAsInt() < 0)
		{
			valid = false;
			result.locations.push_back(std::move(location));
			return;
		}
		location.before = value.GetNumberAsInt();
		if(codecLocal)
		{
			location.after = location.before;
			result.locations.push_back(std::move(location));
			return;
		}
		if(static_cast<std::size_t>(location.before) >= oldToNew.size())
		{
			valid = false;
			result.locations.push_back(std::move(location));
			return;
		}
		const int next = static_cast<int>(oldToNew[static_cast<std::size_t>(location.before)]);
		value = tinygltf::Value(next);
		location.after = next;
		location.rewritten = location.before != next;
		result.locations.push_back(std::move(location));
	}

	void parameterIndex(double& value, std::string path)
	{
		ReferenceLocationInspection location;
		location.jsonPointer = std::move(path);
		if(!std::isfinite(value) || value < 0.0
			|| value > static_cast<double>(std::numeric_limits<int>::max())
			|| std::trunc(value) != value)
		{
			valid = false;
			result.locations.push_back(std::move(location));
			return;
		}
		const int before = static_cast<int>(value);
		location.before = before;
		if(static_cast<std::size_t>(before) >= oldToNew.size())
		{
			valid = false;
			result.locations.push_back(std::move(location));
			return;
		}
		const int next = static_cast<int>(oldToNew[static_cast<std::size_t>(before)]);
		value = static_cast<double>(next);
		location.after = next;
		location.rewritten = before != next;
		result.locations.push_back(std::move(location));
	}

	void materialTextureInfo(
		tinygltf::Value& extension,
		const std::string& path,
		std::string_view member)
	{
		if(!extension.IsObject()) return;
		auto& extensionObject = extension.Get<tinygltf::Value::Object>();
		const auto texture = extensionObject.find(std::string(member));
		if(texture == extensionObject.end()) return;
		const std::string texturePath = path + "." + std::string(member);
		if(!texture->second.IsObject() || !texture->second.Has("index"))
		{
			valid = false;
			ReferenceLocationInspection location;
			location.jsonPointer = texturePath + ".index";
			result.locations.push_back(std::move(location));
			return;
		}
		valueIndex(texture->second.Get<tinygltf::Value::Object>()["index"],
			texturePath + ".index");
	}

	void rewrite()
	{
		if(targetKind == EditorObjectKind::scene)
			index(model.defaultScene, "/scene");
		if(targetKind == EditorObjectKind::node)
		{
			for(std::size_t scene = 0; scene < model.scenes.size(); ++scene)
				for(std::size_t item = 0; item < model.scenes[scene].nodes.size(); ++item)
					index(model.scenes[scene].nodes[item], "/scenes/" + std::to_string(scene)
						+ "/nodes/" + std::to_string(item), false);
			for(std::size_t node = 0; node < model.nodes.size(); ++node)
			{
				for(std::size_t item = 0; item < model.nodes[node].children.size(); ++item)
					index(model.nodes[node].children[item], "/nodes/" + std::to_string(node)
						+ "/children/" + std::to_string(item), false);
				for(std::size_t item = 0; item < model.nodes[node].lods.size(); ++item)
					index(model.nodes[node].lods[item], "/nodes/" + std::to_string(node)
						+ "/lods/" + std::to_string(item), false);
			}
			for(std::size_t skin = 0; skin < model.skins.size(); ++skin)
			{
				index(model.skins[skin].skeleton, "/skins/" + std::to_string(skin) + "/skeleton");
				for(std::size_t item = 0; item < model.skins[skin].joints.size(); ++item)
					index(model.skins[skin].joints[item], "/skins/" + std::to_string(skin)
						+ "/joints/" + std::to_string(item), false);
			}
			for(std::size_t animation = 0; animation < model.animations.size(); ++animation)
				for(std::size_t channel = 0; channel < model.animations[animation].channels.size(); ++channel)
					index(model.animations[animation].channels[channel].target_node,
						"/animations/" + std::to_string(animation) + "/channels/"
						+ std::to_string(channel) + "/target/node");
		}
		if(targetKind == EditorObjectKind::mesh)
			for(std::size_t node = 0; node < model.nodes.size(); ++node)
				index(model.nodes[node].mesh, "/nodes/" + std::to_string(node) + "/mesh");
		if(targetKind == EditorObjectKind::material)
		{
			for(std::size_t material = 0; material < model.materials.size(); ++material)
				for(std::size_t item = 0; item < model.materials[material].lods.size(); ++item)
					index(model.materials[material].lods[item], "/materials/"
						+ std::to_string(material) + "/lods/" + std::to_string(item), false);
			for(std::size_t mesh = 0; mesh < model.meshes.size(); ++mesh)
				for(std::size_t primitive = 0; primitive < model.meshes[mesh].primitives.size(); ++primitive)
				{
					auto& value = model.meshes[mesh].primitives[primitive];
					index(value.material, "/meshes/" + std::to_string(mesh) + "/primitives/"
						+ std::to_string(primitive) + "/material");
					auto variants = value.extensions.find("KHR_materials_variants");
					if(variants != value.extensions.end() && variants->second.IsObject()
						&& variants->second.Has("mappings")
						&& variants->second.Get("mappings").IsArray())
					{
						auto& extensionObject = variants->second.Get<tinygltf::Value::Object>();
						auto& mappings = extensionObject["mappings"].Get<tinygltf::Value::Array>();
						for(std::size_t mapping = 0; mapping < mappings.size(); ++mapping)
							if(mappings[mapping].IsObject() && mappings[mapping].Has("material"))
								valueIndex(mappings[mapping].Get<tinygltf::Value::Object>()["material"],
									"/meshes/" + std::to_string(mesh) + "/primitives/"
									+ std::to_string(primitive) + "/extensions/KHR_materials_variants/mappings/"
									+ std::to_string(mapping) + "/material");
					}
				}
		}
		if(targetKind == EditorObjectKind::texture)
		{
			for(std::size_t material = 0; material < model.materials.size(); ++material)
			{
				auto& value = model.materials[material];
				const std::string base = "/materials/" + std::to_string(material);
				index(value.pbrMetallicRoughness.baseColorTexture.index, base + "/pbrMetallicRoughness/baseColorTexture/index");
				index(value.pbrMetallicRoughness.metallicRoughnessTexture.index, base + "/pbrMetallicRoughness/metallicRoughnessTexture/index");
				index(value.normalTexture.index, base + "/normalTexture/index");
				index(value.occlusionTexture.index, base + "/occlusionTexture/index");
				index(value.emissiveTexture.index, base + "/emissiveTexture/index");
				for(auto& [name, parameter] : value.values)
				{
					auto found = parameter.json_double_value.find("index");
					if(found != parameter.json_double_value.end())
					{
						parameterIndex(found->second,
							base + "/values/" + name + "/index");
					}
				}
				for(auto& [name, parameter] : value.additionalValues)
				{
					auto found = parameter.json_double_value.find("index");
					if(found != parameter.json_double_value.end())
					{
						parameterIndex(found->second,
							base + "/additionalValues/" + name + "/index");
					}
				}
				const auto rewriteExtensionTexture = [&](std::string_view extensionName,
					std::string_view member)
				{
					auto extension = value.extensions.find(std::string(extensionName));
					if(extension != value.extensions.end())
						materialTextureInfo(extension->second,
							base + "/extensions/" + std::string(extensionName), member);
				};
				rewriteExtensionTexture("KHR_materials_anisotropy", "anisotropyTexture");
				rewriteExtensionTexture("KHR_materials_clearcoat", "clearcoatTexture");
				rewriteExtensionTexture("KHR_materials_clearcoat", "clearcoatRoughnessTexture");
				rewriteExtensionTexture("KHR_materials_clearcoat", "clearcoatNormalTexture");
				rewriteExtensionTexture("KHR_materials_diffuse_transmission", "diffuseTransmissionTexture");
				rewriteExtensionTexture("KHR_materials_diffuse_transmission", "diffuseTransmissionColorTexture");
				rewriteExtensionTexture("KHR_materials_iridescence", "iridescenceTexture");
				rewriteExtensionTexture("KHR_materials_iridescence", "iridescenceThicknessTexture");
				rewriteExtensionTexture("KHR_materials_sheen", "sheenColorTexture");
				rewriteExtensionTexture("KHR_materials_sheen", "sheenRoughnessTexture");
				rewriteExtensionTexture("KHR_materials_specular", "specularTexture");
				rewriteExtensionTexture("KHR_materials_specular", "specularColorTexture");
				rewriteExtensionTexture("KHR_materials_transmission", "transmissionTexture");
				rewriteExtensionTexture("KHR_materials_volume", "thicknessTexture");
				rewriteExtensionTexture("KHR_materials_pbrSpecularGlossiness", "diffuseTexture");
				rewriteExtensionTexture("KHR_materials_pbrSpecularGlossiness", "specularGlossinessTexture");
				rewriteExtensionTexture("EXT_materials_bump", "bumpTexture");
			}
		}
		if(targetKind == EditorObjectKind::image)
		{
			static constexpr std::array names{
				"KHR_texture_basisu", "EXT_texture_webp", "EXT_texture_avif", "MSFT_texture_dds"};
			for(std::size_t texture = 0; texture < model.textures.size(); ++texture)
			{
				index(model.textures[texture].source,
					"/textures/" + std::to_string(texture) + "/source");
				for(const char* name : names)
				{
					auto extension = model.textures[texture].extensions.find(name);
					if(extension != model.textures[texture].extensions.end()
						&& extension->second.IsObject() && extension->second.Has("source"))
						valueIndex(extension->second.Get<tinygltf::Value::Object>()["source"],
							"/textures/" + std::to_string(texture) + "/extensions/"
							+ name + "/source");
				}
			}
		}
		if(targetKind == EditorObjectKind::sampler)
			for(std::size_t texture = 0; texture < model.textures.size(); ++texture)
				index(model.textures[texture].sampler,
					"/textures/" + std::to_string(texture) + "/sampler");
		if(targetKind == EditorObjectKind::camera)
			for(std::size_t node = 0; node < model.nodes.size(); ++node)
				index(model.nodes[node].camera, "/nodes/" + std::to_string(node) + "/camera");
		if(targetKind == EditorObjectKind::light)
		{
			for(std::size_t node = 0; node < model.nodes.size(); ++node)
			{
				index(model.nodes[node].light, "/nodes/" + std::to_string(node) + "/light");
				auto extension = model.nodes[node].extensions.find("KHR_lights_punctual");
				if(extension != model.nodes[node].extensions.end() && extension->second.IsObject()
					&& extension->second.Has("light"))
					valueIndex(extension->second.Get<tinygltf::Value::Object>()["light"],
						"/nodes/" + std::to_string(node) + "/extensions/KHR_lights_punctual/light");
			}
		}
		if(targetKind == EditorObjectKind::accessor)
		{
			for(std::size_t mesh = 0; mesh < model.meshes.size(); ++mesh)
				for(std::size_t primitive = 0; primitive < model.meshes[mesh].primitives.size(); ++primitive)
				{
					auto& value = model.meshes[mesh].primitives[primitive];
					const std::string base = "/meshes/" + std::to_string(mesh)
						+ "/primitives/" + std::to_string(primitive);
					index(value.indices, base + "/indices");
					for(auto& [semantic, accessor] : value.attributes)
						index(accessor, base + "/attributes/" + semantic, false);
					for(std::size_t target = 0; target < value.targets.size(); ++target)
						for(auto& [semantic, accessor] : value.targets[target])
							index(accessor, base + "/targets/" + std::to_string(target)
								+ "/" + semantic, false);
					auto draco = value.extensions.find("KHR_draco_mesh_compression");
					if(draco != value.extensions.end() && draco->second.IsObject()
						&& draco->second.Has("attributes") && draco->second.Get("attributes").IsObject())
						for(auto& [semantic, attribute] : draco->second.Get<tinygltf::Value::Object>()["attributes"].Get<tinygltf::Value::Object>())
							valueIndex(attribute, base + "/extensions/KHR_draco_mesh_compression/attributes/"
								+ semantic, true);
				}
			for(std::size_t skin = 0; skin < model.skins.size(); ++skin)
				index(model.skins[skin].inverseBindMatrices,
					"/skins/" + std::to_string(skin) + "/inverseBindMatrices");
			for(std::size_t animation = 0; animation < model.animations.size(); ++animation)
				for(std::size_t sampler = 0; sampler < model.animations[animation].samplers.size(); ++sampler)
				{
					index(model.animations[animation].samplers[sampler].input,
						"/animations/" + std::to_string(animation) + "/samplers/"
						+ std::to_string(sampler) + "/input", false);
					index(model.animations[animation].samplers[sampler].output,
						"/animations/" + std::to_string(animation) + "/samplers/"
						+ std::to_string(sampler) + "/output", false);
				}
			for(std::size_t node = 0; node < model.nodes.size(); ++node)
			{
				auto extension = model.nodes[node].extensions.find("EXT_mesh_gpu_instancing");
				if(extension != model.nodes[node].extensions.end() && extension->second.IsObject()
					&& extension->second.Has("attributes") && extension->second.Get("attributes").IsObject())
					for(auto& [semantic, accessor] : extension->second.Get<tinygltf::Value::Object>()["attributes"].Get<tinygltf::Value::Object>())
						valueIndex(accessor, "/nodes/" + std::to_string(node)
							+ "/extensions/EXT_mesh_gpu_instancing/attributes/" + semantic);
			}
		}
		if(targetKind == EditorObjectKind::bufferView)
		{
			for(std::size_t accessor = 0; accessor < model.accessors.size(); ++accessor)
			{
				auto& value = model.accessors[accessor];
				const std::string base = "/accessors/" + std::to_string(accessor);
				index(value.bufferView, base + "/bufferView");
				if(value.sparse.isSparse)
				{
					index(value.sparse.indices.bufferView, base + "/sparse/indices/bufferView", false);
					index(value.sparse.values.bufferView, base + "/sparse/values/bufferView", false);
				}
			}
			for(std::size_t image = 0; image < model.images.size(); ++image)
				index(model.images[image].bufferView,
					"/images/" + std::to_string(image) + "/bufferView");
			for(std::size_t source = 0; source < model.audioSources.size(); ++source)
				index(model.audioSources[source].bufferView,
					"/audioSources/" + std::to_string(source) + "/bufferView");
			for(std::size_t mesh = 0; mesh < model.meshes.size(); ++mesh)
				for(std::size_t primitive = 0; primitive < model.meshes[mesh].primitives.size(); ++primitive)
				{
					auto draco = model.meshes[mesh].primitives[primitive].extensions.find(
						"KHR_draco_mesh_compression");
					if(draco != model.meshes[mesh].primitives[primitive].extensions.end()
						&& draco->second.IsObject() && draco->second.Has("bufferView"))
						valueIndex(draco->second.Get<tinygltf::Value::Object>()["bufferView"],
							"/meshes/" + std::to_string(mesh) + "/primitives/"
							+ std::to_string(primitive) + "/extensions/KHR_draco_mesh_compression/bufferView");
				}
		}
		if(targetKind == EditorObjectKind::buffer)
		{
			for(std::size_t view = 0; view < model.bufferViews.size(); ++view)
			{
				index(model.bufferViews[view].buffer,
					"/bufferViews/" + std::to_string(view) + "/buffer", false);
				auto extension = model.bufferViews[view].extensions.find("EXT_meshopt_compression");
				if(extension != model.bufferViews[view].extensions.end()
					&& extension->second.IsObject() && extension->second.Has("buffer"))
					valueIndex(extension->second.Get<tinygltf::Value::Object>()["buffer"],
						"/bufferViews/" + std::to_string(view)
						+ "/extensions/EXT_meshopt_compression/buffer");
			}
		}
		if(targetKind == EditorObjectKind::skin)
			for(std::size_t node = 0; node < model.nodes.size(); ++node)
				index(model.nodes[node].skin, "/nodes/" + std::to_string(node) + "/skin");
	}
};

void permuteTargetCollection(
	tinygltf::Model& model, EditorObjectKind kind, const std::vector<std::uint32_t>& map)
{
	switch(kind)
	{
		case EditorObjectKind::scene: applyPermutation(model.scenes, map); break;
		case EditorObjectKind::node: applyPermutation(model.nodes, map); break;
		case EditorObjectKind::primitive:
		{
			std::vector<tinygltf::Primitive> flat;
			std::vector<std::size_t> counts;
			for(auto& mesh : model.meshes)
			{
				counts.push_back(mesh.primitives.size());
				for(auto& primitive : mesh.primitives) flat.push_back(std::move(primitive));
			}
			applyPermutation(flat, map);
			std::size_t offset = 0;
			for(std::size_t mesh = 0; mesh < model.meshes.size(); ++mesh)
			{
				model.meshes[mesh].primitives.clear();
				for(std::size_t item = 0; item < counts[mesh]; ++item)
					model.meshes[mesh].primitives.push_back(std::move(flat[offset++]));
			}
			break;
		}
		case EditorObjectKind::mesh: applyPermutation(model.meshes, map); break;
		case EditorObjectKind::material: applyPermutation(model.materials, map); break;
		case EditorObjectKind::texture: applyPermutation(model.textures, map); break;
		case EditorObjectKind::image: applyPermutation(model.images, map); break;
		case EditorObjectKind::sampler: applyPermutation(model.samplers, map); break;
		case EditorObjectKind::camera: applyPermutation(model.cameras, map); break;
		case EditorObjectKind::light: applyPermutation(model.lights, map); break;
		case EditorObjectKind::accessor: applyPermutation(model.accessors, map); break;
		case EditorObjectKind::bufferView: applyPermutation(model.bufferViews, map); break;
		case EditorObjectKind::buffer: applyPermutation(model.buffers, map); break;
		case EditorObjectKind::skin: applyPermutation(model.skins, map); break;
		case EditorObjectKind::animation: applyPermutation(model.animations, map); break;
		case EditorObjectKind::invalid:
		case EditorObjectKind::count: break;
	}
}

} // namespace

struct GltfDocument::Impl final
{
	struct SceneRootState final
	{
		SceneRootTransform baseline{};
		SceneRootTransform current{};
		std::uint64_t hierarchyEpoch{0};
		std::uint64_t externalStateSerial{0};
		bool frozen{false};
		std::vector<EditorObjectId> frozenRoots;
	};
	struct ObjectIdSlot final
	{
		std::size_t index{std::numeric_limits<std::size_t>::max()};
		std::uint32_t generation{0};
		bool live{false};
	};

	struct ObjectIdRegistry final
	{
		std::vector<EditorObjectId> idsByIndex;
		std::vector<ObjectIdSlot> slots;
	};

	std::unique_ptr<tinygltf::Model> model;
	std::filesystem::path sourcePath;
	GltfDocumentFormat format{GltfDocumentFormat::ascii};
	std::vector<GltfDependency> dependencies;
	EditorDocumentAuthority authority;
	std::uint64_t revision{0};
	std::uint64_t cleanRevision{0};
	SceneId currentScene;
	std::uint64_t sceneActivationEpoch{0};
	std::array<ObjectIdRegistry, kKindCount> ids;
	std::vector<PrimitiveAddress> primitiveAddresses;
	std::vector<std::vector<int>> occurrenceParentsByScene;
	std::vector<SceneRootState> sceneRootTransforms;
	mutable bool semanticDigestCacheValid{false};
	mutable SceneDigest cachedDocumentDigest{};
	mutable std::string cachedSemanticHash;
	mutable bool canonicalPayloadCacheValid{false};
	mutable CanonicalImageUris cachedCanonicalUris;
	mutable bool semanticPrefixCacheValid{false};
	mutable std::string cachedSemanticPrefix;
	mutable std::uint64_t cachedSemanticPrefixHash{14695981039346656037ull};
	mutable SceneSha256Prefix cachedDocumentPrefixHash;
	mutable detail::CanonicalGltfMaterialMetadata cachedMaterialMetadata;
};

[[nodiscard]] bool buildOccurrenceParents(
	const tinygltf::Model& model,
	std::vector<std::vector<int>>& output)
{
	constexpr int absent = -2;
	output.assign(model.scenes.size(), std::vector<int>(model.nodes.size(), absent));
	std::vector<std::pair<int, int>> stack;
	stack.reserve(model.nodes.size());
	for(std::size_t scene = 0; scene < model.scenes.size(); ++scene)
	{
		stack.clear();
		for(auto root = model.scenes[scene].nodes.rbegin();
			root != model.scenes[scene].nodes.rend(); ++root)
			stack.emplace_back(*root, -1);
		while(!stack.empty())
		{
			const auto [node, parent] = stack.back();
			stack.pop_back();
			if(node < 0 || static_cast<std::size_t>(node) >= model.nodes.size()) return false;
			int& recorded = output[scene][static_cast<std::size_t>(node)];
			if(recorded != absent) return false;
			recorded = parent;
			const auto& children = model.nodes[static_cast<std::size_t>(node)].children;
			for(auto child = children.rbegin(); child != children.rend(); ++child)
				stack.emplace_back(*child, node);
		}
	}
	return true;
}

[[nodiscard]] bool finiteSceneRootTransform(const SceneRootTransform& transform) noexcept
{
	for(const double value : transform.translation) if(!std::isfinite(value)) return false;
	for(const double value : transform.rotation) if(!std::isfinite(value)) return false;
	for(const double value : transform.scale) if(!std::isfinite(value)) return false;
	const double length = transform.rotation[0] * transform.rotation[0]
		+ transform.rotation[1] * transform.rotation[1]
		+ transform.rotation[2] * transform.rotation[2]
		+ transform.rotation[3] * transform.rotation[3];
	return length > 1.0e-12 && std::abs(length - 1.0) <= 1.0e-4
		&& std::abs(transform.scale[0]) > 1.0e-12
		&& std::abs(transform.scale[1]) > 1.0e-12
		&& std::abs(transform.scale[2]) > 1.0e-12;
}

[[nodiscard]] SceneRootTransform identitySceneRootTransform() noexcept
{
	return {};
}

const char* editorObjectKindName(EditorObjectKind kind) noexcept
{
	switch(kind)
	{
		case EditorObjectKind::invalid: return "invalid";
		case EditorObjectKind::scene: return "scene";
		case EditorObjectKind::node: return "node";
		case EditorObjectKind::primitive: return "primitive";
		case EditorObjectKind::mesh: return "mesh";
		case EditorObjectKind::material: return "material";
		case EditorObjectKind::texture: return "texture";
		case EditorObjectKind::image: return "image";
		case EditorObjectKind::sampler: return "sampler";
		case EditorObjectKind::camera: return "camera";
		case EditorObjectKind::light: return "light";
		case EditorObjectKind::accessor: return "accessor";
		case EditorObjectKind::bufferView: return "buffer-view";
		case EditorObjectKind::buffer: return "buffer";
		case EditorObjectKind::skin: return "skin";
		case EditorObjectKind::animation: return "animation";
		case EditorObjectKind::count: break;
	}
	return "invalid";
}

GltfDocument::GltfDocument(std::unique_ptr<tinygltf::Model> model,
	                       std::filesystem::path sourcePath,
	                       GltfDocumentFormat format,
	                       std::vector<GltfDependency> dependencies)
	: m_impl(std::make_unique<Impl>())
{
	m_impl->authority = allocateDocumentAuthority();
	m_impl->model = std::move(model);
	m_impl->sourcePath = std::move(sourcePath);
	m_impl->format = format;
	m_impl->dependencies = std::move(dependencies);

	const auto allocateIds = [this](EditorObjectKind kind, std::size_t count)
	{
		auto& registry = m_impl->ids[kindIndex(kind)];
		registry.idsByIndex.reserve(count);
		registry.slots.reserve(count);
		for(std::size_t index = 0; index < count; ++index)
		{
			const auto slot = static_cast<std::uint32_t>(registry.slots.size());
			constexpr std::uint32_t initialGeneration = 1u;
			registry.idsByIndex.push_back(EditorObjectId{
				kind,
				slot,
				initialGeneration,
				m_impl->authority,
			});
			registry.slots.push_back(Impl::ObjectIdSlot{index, initialGeneration, true});
		}
	};

	const tinygltf::Model& authority = *m_impl->model;
	allocateIds(EditorObjectKind::scene, authority.scenes.size());
	allocateIds(EditorObjectKind::node, authority.nodes.size());
	allocateIds(EditorObjectKind::mesh, authority.meshes.size());
	allocateIds(EditorObjectKind::material, authority.materials.size());
	allocateIds(EditorObjectKind::texture, authority.textures.size());
	allocateIds(EditorObjectKind::image, authority.images.size());
	allocateIds(EditorObjectKind::sampler, authority.samplers.size());
	allocateIds(EditorObjectKind::camera, authority.cameras.size());
	allocateIds(EditorObjectKind::light, authority.lights.size());
	allocateIds(EditorObjectKind::accessor, authority.accessors.size());
	allocateIds(EditorObjectKind::bufferView, authority.bufferViews.size());
	allocateIds(EditorObjectKind::buffer, authority.buffers.size());
	allocateIds(EditorObjectKind::skin, authority.skins.size());
	allocateIds(EditorObjectKind::animation, authority.animations.size());

	for(std::size_t meshIndex = 0; meshIndex < authority.meshes.size(); ++meshIndex)
	{
		const tinygltf::Mesh& mesh = authority.meshes[meshIndex];
		for(std::size_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex)
		{
			m_impl->primitiveAddresses.push_back(PrimitiveAddress{
				static_cast<std::uint32_t>(meshIndex),
				static_cast<std::uint32_t>(primitiveIndex),
			});
		}
	}
	allocateIds(EditorObjectKind::primitive, m_impl->primitiveAddresses.size());

	std::size_t initialScene = 0;
	if(authority.defaultScene >= 0
		&& static_cast<std::size_t>(authority.defaultScene) < authority.scenes.size())
	{
		initialScene = static_cast<std::size_t>(authority.defaultScene);
	}
	if(!authority.scenes.empty())
	{
		m_impl->currentScene = SceneId::from(idAt(EditorObjectKind::scene, initialScene)).value();
	}
	m_impl->sceneRootTransforms.resize(authority.scenes.size());
	if(!buildOccurrenceParents(authority, m_impl->occurrenceParentsByScene))
		throw std::logic_error("validated document has an invalid node occurrence table");
}

GltfDocument::~GltfDocument() = default;
GltfDocument::GltfDocument(GltfDocument&&) noexcept = default;
GltfDocument& GltfDocument::operator=(GltfDocument&&) noexcept = default;

const std::filesystem::path& GltfDocument::sourcePath() const noexcept
{
	return m_impl->sourcePath;
}

GltfDocumentFormat GltfDocument::format() const noexcept
{
	return m_impl->format;
}

std::span<const GltfDependency> GltfDocument::dependencies() const noexcept
{
	return m_impl->dependencies;
}

std::uint64_t GltfDocument::revision() const noexcept
{
	return m_impl->revision;
}

std::uint64_t GltfDocument::cleanRevision() const noexcept
{
	return m_impl->cleanRevision;
}

bool GltfDocument::dirty() const noexcept
{
	return revision() != cleanRevision();
}

EditorDocumentAuthority GltfDocument::documentAuthority() const noexcept
{
	return m_impl->authority;
}

SceneDigest GltfDocument::documentDigest() const
{
	if(m_impl->semanticDigestCacheValid)
		return m_impl->cachedDocumentDigest;
	std::string representation;
	if(!m_impl->canonicalPayloadCacheValid)
	{
		m_impl->cachedCanonicalUris.clear();
		m_impl->semanticPrefixCacheValid = false;
	}
	try
	{
		representation =
			m_impl->cachedMaterialMetadata.valid
				? m_impl->cachedMaterialMetadata.text()
				: canonicalMetadata(authorityModel(), m_impl->cachedCanonicalUris,
									m_impl->canonicalPayloadCacheValid, &m_impl->cachedMaterialMetadata);
		m_impl->canonicalPayloadCacheValid = true;
	}
	catch(...)
	{
		m_impl->canonicalPayloadCacheValid = false;
		return {};
	}
	const auto replacements = canonicalUriReplacements(representation, m_impl->cachedCanonicalUris);
	if(!replacements)
	{
		std::string error;
		representation = semanticRepresentation(error);
		if(!error.empty())
			return {};
	}
	const auto span = [](std::string_view bytes)
	{
		return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(bytes.data()),
											 bytes.size());
	};
	std::vector<std::span<const std::uint8_t>> parts;
	std::size_t offset = 0u;
	if(replacements)
		for(const auto &replacement : *replacements)
		{
			parts.push_back(
				span(std::string_view(representation).substr(offset, replacement.position - offset + 1u)));
			parts.push_back(span(*replacement.uri));
			offset = replacement.position + replacement.length - 1u;
		}
	std::uint64_t hash = 14695981039346656037ull;
	// The JSON prefix binds the exact ordering and metadata around immutable
	// payload URIs. Only identity-preserving edits retain these URI owners.
	const std::string_view prefix(representation.data(), offset);
	if(m_impl->semanticPrefixCacheValid && prefix == m_impl->cachedSemanticPrefix)
	{
		hash = m_impl->cachedSemanticPrefixHash;
	}
	else
	{
		for(const auto bytes : parts)
			hash = appendSemanticHash(hash, bytes);
		m_impl->cachedDocumentPrefixHash = SceneChangeCanonicalizer::captureSha256Prefix(parts);
		m_impl->cachedSemanticPrefix.assign(prefix);
		m_impl->cachedSemanticPrefixHash = hash;
		m_impl->semanticPrefixCacheValid = true;
	}
	const auto suffix = span(std::string_view(representation).substr(offset));
	hash = appendSemanticHash(hash, suffix);
	parts.push_back(suffix);
	SceneDigest digest =
		SceneChangeCanonicalizer::computeSha256WithPrefix(m_impl->cachedDocumentPrefixHash, suffix);
	if(!sceneDigestPresent(digest))
		digest = SceneChangeCanonicalizer::computeSha256Parts(parts);
	if(sceneDigestPresent(digest))
	{
		m_impl->cachedDocumentDigest = digest;
		m_impl->cachedSemanticHash = semanticHashHex(hash);
		m_impl->semanticDigestCacheValid = true;
	}
	return digest;
}

std::string GltfDocument::semanticDigest() const
{
	if(!m_impl->semanticDigestCacheValid)
		(void)documentDigest();
	return m_impl->semanticDigestCacheValid ? m_impl->cachedSemanticHash : std::string{};
}

void GltfDocument::invalidateSemanticDigestCache(bool preservePayloads,
												 bool preserveMaterialMetadata) const noexcept
{
	m_impl->semanticDigestCacheValid = false;
	m_impl->cachedDocumentDigest = {};
	m_impl->cachedSemanticHash.clear();
	if(!preserveMaterialMetadata)
		m_impl->cachedMaterialMetadata = {};
	if(!preservePayloads)
	{
		m_impl->canonicalPayloadCacheValid = false;
		m_impl->cachedCanonicalUris.clear();
		m_impl->semanticPrefixCacheValid = false;
		m_impl->cachedSemanticPrefix.clear();
		m_impl->cachedDocumentPrefixHash = {};
	}
}

std::optional<std::size_t> GltfDocument::defaultSceneIndex() const noexcept
{
	const int index = m_impl->model->defaultScene;
	if(index < 0 || static_cast<std::size_t>(index) >= m_impl->model->scenes.size())
	{
		return std::nullopt;
	}
	return static_cast<std::size_t>(index);
}

SceneId GltfDocument::currentSceneId() const noexcept
{
	return m_impl->currentScene;
}

std::optional<SceneId> GltfDocument::activeSceneId() const noexcept
{
	return m_impl->currentScene.valid()
		? std::optional<SceneId>{m_impl->currentScene} : std::nullopt;
}

std::optional<std::size_t> GltfDocument::currentSceneIndex() const noexcept
{
	return indexOf(currentSceneId().untyped());
}

std::uint64_t GltfDocument::sceneActivationEpoch() const noexcept
{
	return m_impl->sceneActivationEpoch;
}

SceneRootTransform GltfDocument::sceneRootTransform(SceneId scene) const noexcept
{
	const auto index = indexOf(scene.untyped());
	if(!index || *index >= m_impl->sceneRootTransforms.size()) return identitySceneRootTransform();
	return m_impl->sceneRootTransforms[*index].current;
}

SceneRootTransform GltfDocument::sceneRootBaseline(SceneId scene) const noexcept
{
	const auto index = indexOf(scene.untyped());
	if(!index || *index >= m_impl->sceneRootTransforms.size()) return identitySceneRootTransform();
	return m_impl->sceneRootTransforms[*index].baseline;
}

SceneRootTransformState GltfDocument::sceneRootTransformState(SceneId scene) const noexcept
{
	SceneRootTransformState result;
	const auto index = indexOf(scene.untyped());
	if(!index || *index >= m_impl->sceneRootTransforms.size()) return result;
	const auto& state = m_impl->sceneRootTransforms[*index];
	result.baseline = state.baseline;
	result.current = state.current;
	result.hierarchyEpoch = state.hierarchyEpoch;
	result.externalStateSerial = state.externalStateSerial;
	result.frozen = state.frozen;
	return result;
}

bool GltfDocument::sceneRootNodeFrozen(SceneId scene, NodeId node) const noexcept
{
	const auto index = indexOf(scene.untyped());
	if(!index || *index >= m_impl->sceneRootTransforms.size()) return false;
	const auto& frozen = m_impl->sceneRootTransforms[*index].frozenRoots;
	return std::find(frozen.begin(), frozen.end(), node.untyped()) != frozen.end();
}

std::vector<NodeId> GltfDocument::frozenSceneRootNodeIds(SceneId scene) const
{
	std::vector<NodeId> result;
	const auto index = indexOf(scene.untyped());
	if(!index || *index >= m_impl->sceneRootTransforms.size()) return result;
	for(const EditorObjectId id : m_impl->sceneRootTransforms[*index].frozenRoots)
		if(const auto typed = NodeId::from(id)) result.push_back(*typed);
	return result;
}

SceneActivationResult GltfDocument::activateCurrentScene(
	std::optional<SceneId> scene) noexcept
{
	SceneActivationResult result;
	result.beforeScene = activeSceneId();
	result.afterScene = result.beforeScene;
	result.baseEpoch = m_impl->sceneActivationEpoch;
	result.targetEpoch = m_impl->sceneActivationEpoch;
	if(scene && !contains(scene->untyped()))
	{
		result.status = SceneActivationStatus::InvalidScene;
		return result;
	}
	if(result.beforeScene == scene)
	{
		result.status = SceneActivationStatus::Unchanged;
		return result;
	}
	std::uint64_t nextEpoch = 0;
	if(!checkedCounterSuccessor(m_impl->sceneActivationEpoch, nextEpoch))
	{
		result.status = SceneActivationStatus::EpochExhausted;
		return result;
	}
	m_impl->currentScene = scene.value_or(SceneId{});
	m_impl->sceneActivationEpoch = nextEpoch;
	result.status = SceneActivationStatus::Changed;
	result.afterScene = scene;
	result.targetEpoch = nextEpoch;
	return result;
}

bool GltfDocument::selectCurrentScene(SceneId scene) noexcept
{
	const SceneActivationResult result = activateCurrentScene(scene);
	return result.status == SceneActivationStatus::Changed
		|| result.status == SceneActivationStatus::Unchanged;
}

#if defined(MGIF_EDITOR_TESTING)
bool GltfDocument::seedSceneActivationEpochForTesting(
	std::uint64_t value) noexcept
{
	m_impl->sceneActivationEpoch = value;
	return true;
}
#endif

detail::MutationCommitStatus GltfDocument::setSceneRootTransform(SceneId scene,
	const SceneRootTransform& transform) noexcept
{
	if(!finiteSceneRootTransform(transform)) return detail::MutationCommitStatus::internalError;
	const auto index = indexOf(scene.untyped());
	if(!index || *index >= m_impl->sceneRootTransforms.size())
		return detail::MutationCommitStatus::internalError;
	auto& state = m_impl->sceneRootTransforms[*index];
	std::uint64_t nextSerial = 0;
	if(!checkedCounterSuccessor(state.externalStateSerial, nextSerial))
		return detail::MutationCommitStatus::externalStateSerialExhausted;
	if(state.frozen)
	{
		state.baseline = identitySceneRootTransform();
		state.frozen = false;
		state.frozenRoots.clear();
	}
	state.current = transform;
	state.externalStateSerial = nextSerial;
	return detail::MutationCommitStatus::applied;
}

detail::MutationCommitStatus GltfDocument::resetSceneRootTransform(SceneId scene) noexcept
{
	const auto index = indexOf(scene.untyped());
	if(!index || *index >= m_impl->sceneRootTransforms.size())
		return detail::MutationCommitStatus::internalError;
	auto& state = m_impl->sceneRootTransforms[*index];
	std::uint64_t nextSerial = 0;
	if(!checkedCounterSuccessor(state.externalStateSerial, nextSerial))
		return detail::MutationCommitStatus::externalStateSerialExhausted;
	state.current = state.baseline;
	state.frozen = false;
	state.frozenRoots.clear();
	state.externalStateSerial = nextSerial;
	return detail::MutationCommitStatus::applied;
}

std::size_t GltfDocument::objectCount(EditorObjectKind kind) const noexcept
{
	return isObjectKind(kind) ? m_impl->ids[kindIndex(kind)].idsByIndex.size() : 0;
}

EditorObjectId GltfDocument::idAt(EditorObjectKind kind, std::size_t index) const noexcept
{
	if(!isObjectKind(kind))
	{
		return {};
	}
	const auto& ids = m_impl->ids[kindIndex(kind)].idsByIndex;
	return index < ids.size() ? ids[index] : EditorObjectId{};
}

std::optional<std::size_t> GltfDocument::indexOf(EditorObjectId id) const noexcept
{
	if(!id.valid() || !isObjectKind(id.kind) || id.authority != m_impl->authority)
	{
		return std::nullopt;
	}
	const auto& registry = m_impl->ids[kindIndex(id.kind)];
	if(id.slot >= registry.slots.size())
	{
		return std::nullopt;
	}
	const Impl::ObjectIdSlot& slot = registry.slots[id.slot];
	if(!slot.live || slot.generation != id.generation
		|| slot.index >= registry.idsByIndex.size()
		|| registry.idsByIndex[slot.index] != id)
	{
		return std::nullopt;
	}
	return slot.index;
}

bool GltfDocument::contains(EditorObjectId id) const noexcept
{
	return indexOf(id).has_value();
}

bool GltfDocument::objectVisible(EditorObjectId id) const noexcept
{
	const auto index = indexOf(id);
	if(!index || id.kind != EditorObjectKind::node || *index >= m_impl->model->nodes.size())
	{
		return true;
	}
	const auto& extensions = m_impl->model->nodes[*index].extensions;
	const auto visibility = extensions.find("KHR_node_visibility");
	if(visibility == extensions.end() || !visibility->second.IsObject())
	{
		return true;
	}
	const tinygltf::Value& value = visibility->second.Get("visible");
	return !value.IsBool() || value.Get<bool>();
}

bool GltfDocument::objectHasVisibility(EditorObjectId id) const noexcept
{
	const auto index = indexOf(id);
	if(!index || id.kind != EditorObjectKind::node || *index >= m_impl->model->nodes.size())
	{
		return false;
	}
	return m_impl->model->nodes[*index].extensions.contains("KHR_node_visibility");
}

std::optional<PrimitiveAddress> GltfDocument::primitiveAddress(PrimitiveId id) const noexcept
{
	const auto index = indexOf(id.untyped());
	if(!index || *index >= m_impl->primitiveAddresses.size())
	{
		return std::nullopt;
	}
	return m_impl->primitiveAddresses[*index];
}

std::optional<PrimitiveId> GltfDocument::primitiveIdAt(PrimitiveAddress address) const noexcept
{
	const auto found = std::lower_bound(
		m_impl->primitiveAddresses.begin(), m_impl->primitiveAddresses.end(), address);
	if(found == m_impl->primitiveAddresses.end() || *found != address)
	{
		return std::nullopt;
	}
	const std::size_t index = static_cast<std::size_t>(found - m_impl->primitiveAddresses.begin());
	return PrimitiveId::from(idAt(EditorObjectKind::primitive, index));
}

std::optional<PrimitiveGeometryStats>
GltfDocument::primitiveGeometryStats(PrimitiveId id) const noexcept
{
	const auto address = primitiveAddress(id);
	if(!address || address->meshIndex >= m_impl->model->meshes.size())
		return std::nullopt;
	const tinygltf::Mesh& mesh = m_impl->model->meshes[address->meshIndex];
	if(address->primitiveIndex >= mesh.primitives.size())
		return std::nullopt;
	const tinygltf::Primitive& primitive = mesh.primitives[address->primitiveIndex];
	const auto position = primitive.attributes.find("POSITION");
	if(position == primitive.attributes.end() || position->second < 0 ||
		static_cast<std::size_t>(position->second) >= m_impl->model->accessors.size())
		return std::nullopt;
	const std::size_t vertexCount =
		m_impl->model->accessors[static_cast<std::size_t>(position->second)].count;
	std::size_t indexCount = vertexCount;
	if(primitive.indices >= 0) {
		if(static_cast<std::size_t>(primitive.indices) >= m_impl->model->accessors.size())
			return std::nullopt;
		indexCount = m_impl->model->accessors[static_cast<std::size_t>(primitive.indices)].count;
	}
	if(vertexCount > std::numeric_limits<std::uint32_t>::max() ||
		indexCount > std::numeric_limits<std::uint32_t>::max())
		return std::nullopt;
	return PrimitiveGeometryStats{static_cast<std::uint32_t>(vertexCount),
		static_cast<std::uint32_t>(indexCount)};
}

std::optional<std::array<double, 4>> GltfDocument::materialBaseColor(MaterialId material) const noexcept
{
	const auto index = indexOf(material.untyped());
	if(!index) return std::nullopt;
	const auto& color = authorityModel().materials[*index].pbrMetallicRoughness.baseColorFactor;
	if(color.size() != 4u) return std::nullopt;
	return std::array<double, 4>{color[0], color[1], color[2], color[3]};
}

std::optional<SceneMaterialParametersValue>
GltfDocument::materialParameters(MaterialId material) const noexcept
{
	const auto index = indexOf(material.untyped());
	if(!index) return std::nullopt;
	const tinygltf::Material& source = authorityModel().materials[*index];
	const auto& pbr = source.pbrMetallicRoughness;
	if(pbr.baseColorFactor.size() != 4u || source.emissiveFactor.size() != 3u)
		return std::nullopt;
	SceneMaterialParametersValue result;
	std::copy_n(pbr.baseColorFactor.begin(), 4u, result.baseColorFactor.begin());
	result.metallicFactor = pbr.metallicFactor;
	result.roughnessFactor = pbr.roughnessFactor;
	std::copy_n(source.emissiveFactor.begin(), 3u, result.emissiveFactor.begin());
	result.alphaCutoff = source.alphaCutoff;
	result.alphaMode = source.alphaMode == "MASK" ? SceneAlphaMode::Mask
		: source.alphaMode == "BLEND" ? SceneAlphaMode::Blend : SceneAlphaMode::Opaque;
	result.doubleSided = source.doubleSided;
	return result;
}

std::optional<SceneTextureBindingValue> GltfDocument::materialTextureBinding(
	MaterialId material, SceneTextureSemantic semantic) const noexcept
{
	const auto index = indexOf(material.untyped());
	if(!index) return std::nullopt;
	const tinygltf::Material& source = authorityModel().materials[*index];
	const tinygltf::NormalTextureInfo* normal = nullptr;
	const tinygltf::OcclusionTextureInfo* occlusion = nullptr;
	if(semantic == SceneTextureSemantic::Normal)
		normal = &source.normalTexture;
	else if(semantic == SceneTextureSemantic::Occlusion)
		occlusion = &source.occlusionTexture;
	else
		return std::nullopt;
	const int textureIndex = normal ? normal->index : occlusion->index;
	const int uvSet = normal ? normal->texCoord : occlusion->texCoord;
	if(uvSet < 0) return std::nullopt;
	SceneTextureBindingValue result;
	result.semantic = semantic;
	result.uvSet = static_cast<std::uint32_t>(uvSet);
	result.scalar = normal ? normal->scale : occlusion->strength;
	if(textureIndex >= 0) {
		const EditorObjectId texture = idAt(EditorObjectKind::texture,
			static_cast<std::size_t>(textureIndex));
		if(!texture.valid() || static_cast<std::size_t>(textureIndex) >= authorityModel().textures.size())
			return std::nullopt;
		result.texture = stableSceneId(texture);
		const int samplerIndex = authorityModel().textures[
			static_cast<std::size_t>(textureIndex)].sampler;
		if(samplerIndex >= 0) {
			const EditorObjectId sampler = idAt(EditorObjectKind::sampler,
				static_cast<std::size_t>(samplerIndex));
			if(!sampler.valid()) return std::nullopt;
			result.sampler = stableSceneId(sampler);
		}
	}
	return result;
}

bool GltfDocument::materialHasExtension(MaterialId material, std::string_view name) const noexcept
{
	const auto index = indexOf(material.untyped());
	if(!index) return false;
	const auto& extensions = authorityModel().materials[*index].extensions;
	return std::any_of(extensions.begin(), extensions.end(),
		[name](const auto& entry) { return entry.first == name; });
}

std::string_view GltfDocument::objectName(EditorObjectId id) const noexcept
{
	const auto index = indexOf(id);
	if(!index)
	{
		return {};
	}
	const tinygltf::Model& model = *m_impl->model;
	switch(id.kind)
	{
		case EditorObjectKind::scene: return model.scenes[*index].name;
		case EditorObjectKind::node: return model.nodes[*index].name;
		case EditorObjectKind::mesh: return model.meshes[*index].name;
		case EditorObjectKind::material: return model.materials[*index].name;
		case EditorObjectKind::texture: return model.textures[*index].name;
		case EditorObjectKind::image: return model.images[*index].name;
		case EditorObjectKind::sampler: return model.samplers[*index].name;
		case EditorObjectKind::camera: return model.cameras[*index].name;
		case EditorObjectKind::light: return model.lights[*index].name;
		case EditorObjectKind::accessor: return model.accessors[*index].name;
		case EditorObjectKind::bufferView: return model.bufferViews[*index].name;
		case EditorObjectKind::buffer: return model.buffers[*index].name;
		case EditorObjectKind::skin: return model.skins[*index].name;
		case EditorObjectKind::animation: return model.animations[*index].name;
		case EditorObjectKind::primitive:
		case EditorObjectKind::invalid:
		case EditorObjectKind::count:
			break;
	}
	return {};
}

std::vector<NodeId> GltfDocument::sceneRootNodeIds(SceneId scene) const
{
	std::vector<NodeId> result;
	const auto sceneIndex = indexOf(scene.untyped());
	if(!sceneIndex)
	{
		return result;
	}
	const auto& roots = m_impl->model->scenes[*sceneIndex].nodes;
	result.reserve(roots.size());
	for(const int root : roots)
	{
		if(root >= 0)
		{
			const auto typed = NodeId::from(idAt(EditorObjectKind::node, static_cast<std::size_t>(root)));
			if(typed)
			{
				result.push_back(*typed);
			}
		}
	}
	return result;
}

std::vector<NodeId> GltfDocument::nodeChildIds(NodeId node) const
{
	std::vector<NodeId> result;
	const auto nodeIndex = indexOf(node.untyped());
	if(!nodeIndex)
	{
		return result;
	}
	const auto& children = m_impl->model->nodes[*nodeIndex].children;
	result.reserve(children.size());
	for(const int child : children)
	{
		if(child >= 0)
		{
			const auto typed = NodeId::from(idAt(EditorObjectKind::node, static_cast<std::size_t>(child)));
			if(typed)
			{
				result.push_back(*typed);
			}
		}
	}
	return result;
}

std::vector<NodeId> GltfDocument::sceneNodeParentPath(
	SceneId scene, NodeId node) const
{
	std::vector<NodeId> result;
	const auto sceneIndex = indexOf(scene.untyped());
	const auto nodeIndex = indexOf(node.untyped());
	if(!sceneIndex || !nodeIndex ||
	   *sceneIndex >= m_impl->occurrenceParentsByScene.size() ||
	   *nodeIndex >= m_impl->occurrenceParentsByScene[*sceneIndex].size())
		return result;

	const auto &parents = m_impl->occurrenceParentsByScene[*sceneIndex];
	std::size_t current = *nodeIndex;
	std::size_t guard = 0u;
	while(current < parents.size() && guard++ <= parents.size())
	{
		const int parent = parents[current];
		if(parent < 0)
			break;
		const auto parentId = NodeId::from(idAt(
			EditorObjectKind::node, static_cast<std::size_t>(parent)));
		if(!parentId)
		{
			result.clear();
			break;
		}
		result.push_back(*parentId);
		current = static_cast<std::size_t>(parent);
	}
	std::reverse(result.begin(), result.end());
	return result;
}

bool GltfDocument::hasNodeOccurrence(
	SceneId scene, NodeId node, std::optional<NodeId> directParent) const noexcept
{
	const auto sceneIndex = indexOf(scene.untyped());
	const auto nodeIndex = indexOf(node.untyped());
	if(!sceneIndex || !nodeIndex) return false;
	if(*sceneIndex >= m_impl->occurrenceParentsByScene.size()
		|| *nodeIndex >= m_impl->occurrenceParentsByScene[*sceneIndex].size()) return false;
	const int actualParent = m_impl->occurrenceParentsByScene[*sceneIndex][*nodeIndex];
	if(!directParent) return actualParent == -1;
	const auto parentIndex = indexOf(directParent->untyped());
	return parentIndex && *parentIndex <= static_cast<std::size_t>(std::numeric_limits<int>::max())
		&& actualParent == static_cast<int>(*parentIndex);
}

bool GltfDocument::ownsPrimitiveOccurrence(
	SceneId scene, NodeId node, MeshId mesh, PrimitiveId primitive) const noexcept
{
	const auto sceneIndex = indexOf(scene.untyped());
	const auto nodeIndex = indexOf(node.untyped());
	const auto meshIndex = indexOf(mesh.untyped());
	const auto address = primitiveAddress(primitive);
	if(!sceneIndex || !nodeIndex || !meshIndex || !address
		|| address->meshIndex != *meshIndex) return false;
	const tinygltf::Model& model = *m_impl->model;
	if(*sceneIndex >= model.scenes.size() || *nodeIndex >= model.nodes.size()
		|| *meshIndex >= model.meshes.size()) return false;
	if(model.nodes[*nodeIndex].mesh != static_cast<int>(*meshIndex)
		|| address->primitiveIndex >= model.meshes[*meshIndex].primitives.size()) return false;
	return *sceneIndex < m_impl->occurrenceParentsByScene.size()
		&& *nodeIndex < m_impl->occurrenceParentsByScene[*sceneIndex].size()
		&& m_impl->occurrenceParentsByScene[*sceneIndex][*nodeIndex] != -2;
}

std::string_view GltfDocument::documentExtensionsJson() const noexcept
{
	return m_impl->model->extensions_json_string;
}

std::string_view GltfDocument::documentExtrasJson() const noexcept
{
	return m_impl->model->extras_json_string;
}

std::string_view GltfDocument::assetVersion() const noexcept
{
	return m_impl->model->asset.version;
}

std::string_view GltfDocument::assetMinVersion() const noexcept
{
	return m_impl->model->asset.minVersion;
}

std::string_view GltfDocument::assetGenerator() const noexcept
{
	return m_impl->model->asset.generator;
}

std::string_view GltfDocument::assetCopyright() const noexcept
{
	return m_impl->model->asset.copyright;
}

std::vector<std::string> GltfDocument::documentExtensionNames() const
{
	std::vector<std::string> result;
	result.reserve(m_impl->model->extensionsUsed.size()
		+ m_impl->model->extensionsRequired.size()
		+ m_impl->model->extensions.size());
	const auto appendUnique = [&result](const std::string& name)
	{
		if(name.empty() || std::find(result.begin(), result.end(), name) != result.end())
			return;
		result.push_back(name);
	};
	// The declaration arrays are authored order and are the authoritative list
	// even when an extension has no root payload object.
	for(const std::string& name : m_impl->model->extensionsUsed) appendUnique(name);
	for(const std::string& name : m_impl->model->extensionsRequired) appendUnique(name);
	for(const auto& entry : m_impl->model->extensions)
	{
		appendUnique(entry.first);
	}
	return result;
}

std::vector<std::string> GltfDocument::assetExtensionNames() const
{
	std::vector<std::string> result;
	result.reserve(m_impl->model->asset.extensions.size());
	for(const auto& entry : m_impl->model->asset.extensions)
	{
		result.push_back(entry.first);
	}
	return result;
}

std::string_view GltfDocument::assetExtensionsJson() const noexcept
{
	return m_impl->model->asset.extensions_json_string;
}

std::string_view GltfDocument::assetExtrasJson() const noexcept
{
	return m_impl->model->asset.extras_json_string;
}

std::string_view GltfDocument::objectExtensionsJson(EditorObjectId id) const noexcept
{
	const auto index = indexOf(id);
	if(!index)
	{
		return {};
	}
	const tinygltf::Model& model = *m_impl->model;
	switch(id.kind)
	{
		case EditorObjectKind::scene: return model.scenes[*index].extensions_json_string;
		case EditorObjectKind::node: return model.nodes[*index].extensions_json_string;
		case EditorObjectKind::mesh: return model.meshes[*index].extensions_json_string;
		case EditorObjectKind::material: return model.materials[*index].extensions_json_string;
		case EditorObjectKind::texture: return model.textures[*index].extensions_json_string;
		case EditorObjectKind::image: return model.images[*index].extensions_json_string;
		case EditorObjectKind::sampler: return model.samplers[*index].extensions_json_string;
		case EditorObjectKind::camera: return model.cameras[*index].extensions_json_string;
		case EditorObjectKind::light: return model.lights[*index].extensions_json_string;
		case EditorObjectKind::accessor: return model.accessors[*index].extensions_json_string;
		case EditorObjectKind::bufferView: return model.bufferViews[*index].extensions_json_string;
		case EditorObjectKind::buffer: return model.buffers[*index].extensions_json_string;
		case EditorObjectKind::skin: return model.skins[*index].extensions_json_string;
		case EditorObjectKind::animation: return model.animations[*index].extensions_json_string;
		case EditorObjectKind::primitive:
		{
			const PrimitiveAddress address = m_impl->primitiveAddresses[*index];
			return model.meshes[address.meshIndex].primitives[address.primitiveIndex].extensions_json_string;
		}
		case EditorObjectKind::invalid:
		case EditorObjectKind::count:
			break;
	}
	return {};
}

std::string_view GltfDocument::objectExtrasJson(EditorObjectId id) const noexcept
{
	const auto index = indexOf(id);
	if(!index)
	{
		return {};
	}
	const tinygltf::Model& model = *m_impl->model;
	switch(id.kind)
	{
		case EditorObjectKind::scene: return model.scenes[*index].extras_json_string;
		case EditorObjectKind::node: return model.nodes[*index].extras_json_string;
		case EditorObjectKind::mesh: return model.meshes[*index].extras_json_string;
		case EditorObjectKind::material: return model.materials[*index].extras_json_string;
		case EditorObjectKind::texture: return model.textures[*index].extras_json_string;
		case EditorObjectKind::image: return model.images[*index].extras_json_string;
		case EditorObjectKind::sampler: return model.samplers[*index].extras_json_string;
		case EditorObjectKind::camera: return model.cameras[*index].extras_json_string;
		case EditorObjectKind::light: return model.lights[*index].extras_json_string;
		case EditorObjectKind::accessor: return model.accessors[*index].extras_json_string;
		case EditorObjectKind::bufferView: return model.bufferViews[*index].extras_json_string;
		case EditorObjectKind::buffer: return model.buffers[*index].extras_json_string;
		case EditorObjectKind::skin: return model.skins[*index].extras_json_string;
		case EditorObjectKind::animation: return model.animations[*index].extras_json_string;
		case EditorObjectKind::primitive:
		{
			const PrimitiveAddress address = m_impl->primitiveAddresses[*index];
			return model.meshes[address.meshIndex].primitives[address.primitiveIndex].extras_json_string;
		}
		case EditorObjectKind::invalid:
		case EditorObjectKind::count:
			break;
	}
	return {};
}

const tinygltf::Model& GltfDocument::authorityModel() const noexcept
{
	return *m_impl->model;
}

tinygltf::Model& GltfDocument::mutableAuthorityModel() noexcept
{
	invalidateSemanticDigestCache();
	return *m_impl->model;
}

std::unique_ptr<tinygltf::Model> GltfDocument::takeAuthorityModel() noexcept
{
	invalidateSemanticDigestCache();
	return std::move(m_impl->model);
}

std::string GltfDocument::semanticRepresentation(std::string& error) const
{
	return semanticRepresentation(authorityModel(), error);
}

std::string GltfDocument::semanticRepresentation(
	const tinygltf::Model& model, std::string& error)
{
	try
	{
		tinygltf::Model canonical = model;
		const auto normalize = [](tinygltf::Model& copy)
		{
			for(auto& buffer : copy.buffers) buffer.uri.clear();
			for(std::size_t index = 0; index < copy.images.size(); ++index)
			{
				auto& image = copy.images[index];
				if(image.bufferView < 0)
					image.uri = "canonical-image-" + std::to_string(index) + "." + canonicalImageExtension(image);
			}
		};
		normalize(canonical);

		CanonicalImageUris imageUris;
		for(auto& buffer : canonical.buffers)
		{
			buffer.uri = canonicalImagePlaceholder(imageUris.size());
			imageUris.push_back(canonicalPayloadUri(buffer.data, "application/octet-stream"));
		}
		for(auto& image : canonical.images)
		{
			if(!image.uri.empty() || image.bufferView < 0)
			{
				std::string uri;
				if(!writeCanonicalImage(nullptr, nullptr, &image, true, nullptr, nullptr, &uri, &imageUris))
				{
					error = "Could not encode a canonical image URI";
					return {};
				}
				image.uri = std::move(uri);
			}
		}
		std::ostringstream stream(std::ios::out | std::ios::binary);
		if(!detail::writeCanonicalGltfMetadata(canonical, stream) || !stream.good())
		{
			error = "TinyGLTF could not serialize the canonical document representation";
			return {};
		}
		std::string representation = stream.str();
		if(expandCanonicalImageUris(representation, imageUris)) return representation;
		canonical = model;
		normalize(canonical);
		tinygltf::TinyGLTF writer;
		writer.SetImageWriter(&writeCanonicalImage, nullptr);
		stream.str({});
		stream.clear();
		if(!writer.WriteGltfSceneToStream(&canonical, stream, false, false) || !stream.good())
		{
			error = "TinyGLTF could not serialize the canonical document representation";
			return {};
		}
		return stream.str();
	}
	catch(const std::exception& exception)
	{
		error = exception.what();
		return {};
	}
	catch(...)
	{
		error = "unknown canonical serialization failure";
		return {};
	}
}

SceneDigest GltfDocument::documentDigest(const tinygltf::Model& model)
{
	std::string error;
	const std::string bytes = semanticRepresentation(model, error);
	if(!error.empty()) return {};
	return SceneChangeCanonicalizer::computeSha256(
		std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

bool GltfDocument::advanceRevision() noexcept
{
	std::uint64_t next = 0;
	if(!checkedCounterSuccessor(m_impl->revision, next)) return false;
	m_impl->revision = next;
	return true;
}

detail::MutationCommitStatus GltfDocument::resolvePreparedMutationIds(
	detail::MutationRegistryPlan& plan) const noexcept
{
	try
	{
		for(std::size_t kind = 0; kind < kKindCount; ++kind)
		{
			const auto& old = m_impl->ids[kind];
			std::vector<bool> retained(old.slots.size(), false);
			std::vector<bool> assigned(old.slots.size(), false);
			std::size_t nextSlot = old.slots.size();
			for(EditorObjectId& requested : plan.idsByKind[kind])
			{
				const bool live = requested.valid()
					&& requested.authority == m_impl->authority
					&& requested.kind == static_cast<EditorObjectKind>(kind)
					&& requested.slot < old.slots.size()
					&& old.slots[requested.slot].live
					&& old.slots[requested.slot].generation == requested.generation
					&& old.slots[requested.slot].index < old.idsByIndex.size()
					&& old.idsByIndex[old.slots[requested.slot].index] == requested;
				const bool resurrected = plan.allowResurrection && requested.valid()
					&& requested.authority == m_impl->authority
					&& requested.kind == static_cast<EditorObjectKind>(kind)
					&& requested.slot < old.slots.size()
					&& !old.slots[requested.slot].live
					&& requested.generation != std::numeric_limits<std::uint32_t>::max()
					&& old.slots[requested.slot].generation == requested.generation + 1u;
				if(live || resurrected)
				{
					if(assigned[requested.slot])
						return detail::MutationCommitStatus::internalError;
					assigned[requested.slot] = true;
					retained[requested.slot] = live;
					continue;
				}
				if(nextSlot > std::numeric_limits<std::uint32_t>::max())
					return detail::MutationCommitStatus::internalError;
				requested = EditorObjectId{
					static_cast<EditorObjectKind>(kind),
					static_cast<std::uint32_t>(nextSlot++), 1u, m_impl->authority};
			}
			for(std::size_t slot = 0; slot < old.slots.size(); ++slot)
			{
				if(!old.slots[slot].live || retained[slot]) continue;
				std::uint32_t next = 0;
				if(!checkedCounterSuccessor(old.slots[slot].generation, next))
					return detail::MutationCommitStatus::generationExhausted;
			}
		}
		return detail::MutationCommitStatus::applied;
	}
	catch(const std::bad_alloc&)
	{
		return detail::MutationCommitStatus::allocationFailed;
	}
	catch(...)
	{
		return detail::MutationCommitStatus::internalError;
	}
}

detail::MutationCommitStatus GltfDocument::commitMutation(
	std::unique_ptr<tinygltf::Model> model,
	detail::MutationRegistryPlan& plan,
	detail::MutationPrePublishCallback prePublish,
	void* prePublishContext) noexcept
{
	if(!model)
	{
		return detail::MutationCommitStatus::internalError;
	}
	std::uint64_t nextRevision = 0;
	if(!checkedCounterSuccessor(m_impl->revision, nextRevision))
		return detail::MutationCommitStatus::revisionExhausted;

	bool invokingPrePublish = false;
	try
	{
		for(std::size_t kind = 1; kind < kKindCount; ++kind)
		{
			if(plan.idsByKind[kind].size()
				!= modelObjectCount(*model, static_cast<EditorObjectKind>(kind)))
			{
				return detail::MutationCommitStatus::internalError;
			}
		}
		std::array<Impl::ObjectIdRegistry, kKindCount> nextIds;
		for(std::size_t kind = 0; kind < kKindCount; ++kind)
		{
			nextIds[kind].slots = m_impl->ids[kind].slots;
			nextIds[kind].idsByIndex.reserve(plan.idsByKind[kind].size());
		}

		for(std::size_t kind = 0; kind < kKindCount; ++kind)
		{
			const auto& requested = plan.idsByKind[kind];
			const auto& old = m_impl->ids[kind];
			std::vector<bool> oldLive(old.slots.size(), false);
			std::vector<bool> assigned(old.slots.size(), false);
			for(const EditorObjectId requestedId : requested)
			{
				EditorObjectId id = requestedId;
				if(id.valid() && id.authority == m_impl->authority
					&& id.kind == static_cast<EditorObjectKind>(kind)
					&& id.slot < old.slots.size()
					&& old.slots[id.slot].live
					&& old.slots[id.slot].generation == id.generation
					&& old.slots[id.slot].index < old.idsByIndex.size()
					&& old.idsByIndex[old.slots[id.slot].index] == id)
				{
					if(assigned[id.slot]) return detail::MutationCommitStatus::internalError;
					assigned[id.slot] = true;
					oldLive[id.slot] = true;
				}
				else if(plan.allowResurrection && id.valid()
					&& id.authority == m_impl->authority
					&& id.kind == static_cast<EditorObjectKind>(kind)
					&& id.slot < old.slots.size()
					&& !old.slots[id.slot].live
					&& id.generation != std::numeric_limits<std::uint32_t>::max()
					&& old.slots[id.slot].generation == id.generation + 1u)
				{
					if(assigned[id.slot]) return detail::MutationCommitStatus::internalError;
					assigned[id.slot] = true;
					auto& resurrected = nextIds[kind].slots[id.slot];
					resurrected.generation = id.generation;
					resurrected.live = true;
				}
				else
				{
					id = {};
				}

				if(!id.valid())
				{
					if(nextIds[kind].slots.size() > std::numeric_limits<std::uint32_t>::max())
					{
						return detail::MutationCommitStatus::internalError;
					}
					 id = EditorObjectId{
						static_cast<EditorObjectKind>(kind),
						static_cast<std::uint32_t>(nextIds[kind].slots.size()),
						1u,
					m_impl->authority};
					nextIds[kind].slots.push_back(Impl::ObjectIdSlot{
						nextIds[kind].idsByIndex.size(), 1u, true});
				}
				const std::size_t newIndex = nextIds[kind].idsByIndex.size();
				nextIds[kind].idsByIndex.push_back(id);
				if(id.slot < nextIds[kind].slots.size())
				{
					nextIds[kind].slots[id.slot].index = newIndex;
					nextIds[kind].slots[id.slot].live = true;
				}
			}

			// Every old live slot omitted from the plan becomes a tombstone. The
			// generation is advanced without wrapping; exhausted slots remain
			// permanently stale and are never reused.
			for(std::size_t slot = 0; slot < old.slots.size(); ++slot)
			{
				if(old.slots[slot].live && !oldLive[slot])
				{
					auto& tombstone = nextIds[kind].slots[slot];
					std::uint32_t nextGeneration = 0;
					if(!checkedCounterSuccessor(tombstone.generation, nextGeneration))
						return detail::MutationCommitStatus::generationExhausted;
					tombstone.index = std::numeric_limits<std::size_t>::max();
					tombstone.generation = nextGeneration;
					tombstone.live = false;
				}
			}
		}

		for(std::size_t kind = 0; kind < kKindCount; ++kind)
		{
			plan.idsByKind[kind] = nextIds[kind].idsByIndex;
		}
		if(plan.primitiveAddresses.size() != nextIds[kindIndex(EditorObjectKind::primitive)].idsByIndex.size())
		{
			return detail::MutationCommitStatus::internalError;
		}
		std::vector<PrimitiveAddress> nextPrimitiveAddresses = plan.primitiveAddresses;
		std::vector<std::vector<int>> nextOccurrenceParents;
		if(!buildOccurrenceParents(*model, nextOccurrenceParents))
			return detail::MutationCommitStatus::internalError;

		SceneId nextCurrent = plan.currentScene;
		const auto currentIsLive = [&]() noexcept
		{
			if(!nextCurrent.valid() || nextCurrent.authority() != m_impl->authority
				|| nextCurrent.untyped().kind != EditorObjectKind::scene)
			{
				return false;
			}
			const auto& scenes = nextIds[kindIndex(EditorObjectKind::scene)].idsByIndex;
			return std::find(scenes.begin(), scenes.end(), nextCurrent.untyped()) != scenes.end();
					};
		if(!currentIsLive())
		{
			// Resolve the requested current scene against the newly built scene
			// registry without relying on the old index.
			nextCurrent = {};
			const auto& scenes = nextIds[kindIndex(EditorObjectKind::scene)].idsByIndex;
			if(!scenes.empty())
			{
				const std::size_t preferred = model->defaultScene >= 0
					&& static_cast<std::size_t>(model->defaultScene) < scenes.size()
					? static_cast<std::size_t>(model->defaultScene) : 0u;
				nextCurrent = *SceneId::from(scenes[preferred]);
			}
		}
		std::uint64_t nextSceneActivationEpoch = m_impl->sceneActivationEpoch;
		if(nextCurrent != m_impl->currentScene
			&& !checkedCounterSuccessor(
				m_impl->sceneActivationEpoch, nextSceneActivationEpoch))
		{
			return detail::MutationCommitStatus::sceneActivationEpochExhausted;
		}

		std::vector<Impl::SceneRootState> nextSceneRootTransforms;
		nextSceneRootTransforms.resize(model->scenes.size());
		const auto& oldSceneIds = m_impl->ids[kindIndex(EditorObjectKind::scene)].idsByIndex;
		const auto& nextSceneIds = nextIds[kindIndex(EditorObjectKind::scene)].idsByIndex;
		const auto& oldNodeIds = m_impl->ids[kindIndex(EditorObjectKind::node)].idsByIndex;
		const auto& nextNodeIds = nextIds[kindIndex(EditorObjectKind::node)].idsByIndex;
		if(m_impl->occurrenceParentsByScene.size() != oldSceneIds.size()
			|| nextOccurrenceParents.size() != nextSceneIds.size())
			return detail::MutationCommitStatus::internalError;
		const auto occurrenceChanged = [&](std::size_t oldSceneIndex,
			std::size_t nextSceneIndex) noexcept -> std::optional<bool>
		{
			if(oldSceneIndex >= m_impl->occurrenceParentsByScene.size()
				|| nextSceneIndex >= nextOccurrenceParents.size()) return std::nullopt;
			const auto parentId = [](int parent, const auto& ids) noexcept
				-> std::optional<EditorObjectId>
			{
				if(parent == -1) return EditorObjectId{};
				if(parent < 0 || static_cast<std::size_t>(parent) >= ids.size())
					return std::nullopt;
				return ids[static_cast<std::size_t>(parent)];
			};
			const auto& oldParents = m_impl->occurrenceParentsByScene[oldSceneIndex];
			const auto& nextParents = nextOccurrenceParents[nextSceneIndex];
			if(oldParents.size() != oldNodeIds.size()
				|| nextParents.size() != nextNodeIds.size()) return std::nullopt;
			const auto& nextNodeRegistry = nextIds[kindIndex(EditorObjectKind::node)];
			const auto nextIndex = [&](EditorObjectId id) noexcept
				-> std::optional<std::size_t>
			{
				if(id.slot >= nextNodeRegistry.slots.size()) return std::nullopt;
				const auto& slot = nextNodeRegistry.slots[id.slot];
				if(!slot.live || slot.generation != id.generation
					|| slot.index >= nextNodeRegistry.idsByIndex.size()
					|| nextNodeRegistry.idsByIndex[slot.index] != id) return std::nullopt;
				return slot.index;
			};
			for(std::size_t oldNode = 0; oldNode < oldNodeIds.size(); ++oldNode)
			{
				const auto nextNode = nextIndex(oldNodeIds[oldNode]);
				const bool oldPresent = oldNode < oldParents.size() && oldParents[oldNode] != -2;
				const bool nextPresent = nextNode && nextParents[*nextNode] != -2;
				if(oldPresent != nextPresent) return true;
				if(oldPresent)
				{
					const auto oldParent = parentId(oldParents[oldNode], oldNodeIds);
					const auto nextParent = parentId(nextParents[*nextNode], nextNodeIds);
					if(!oldParent || !nextParent) return std::nullopt;
					if(*oldParent != *nextParent) return true;
				}
			}
			for(std::size_t nextNode = 0; nextNode < nextNodeIds.size(); ++nextNode)
				if(nextParents[nextNode] != -2
					&& !indexOf(nextNodeIds[nextNode])) return true;
			return false;
		};
		for(std::size_t index = 0; index < nextSceneRootTransforms.size(); ++index)
		{
			const EditorObjectId sceneId = nextSceneIds[index];
			const auto oldScene = std::find(oldSceneIds.begin(), oldSceneIds.end(), sceneId);
			const std::size_t oldIndex = static_cast<std::size_t>(oldScene - oldSceneIds.begin());
			const bool hadOldState = oldScene != oldSceneIds.end()
				&& oldIndex < m_impl->sceneRootTransforms.size();
			if(!plan.hierarchyChanged && hadOldState)
			{
				nextSceneRootTransforms[index] = m_impl->sceneRootTransforms[oldIndex];
				continue;
			}

			auto& nextState = nextSceneRootTransforms[index];
			if(hadOldState)
			{
				if(!checkedCounterSuccessor(
					m_impl->sceneRootTransforms[oldIndex].hierarchyEpoch,
					nextState.hierarchyEpoch))
					return detail::MutationCommitStatus::hierarchyEpochExhausted;
			}
			else
			{
				nextState.hierarchyEpoch = plan.hierarchyChanged ? 1u : 0u;
			}
			if(index < plan.frozenSceneRootIndicesByScene.size())
			{
				const auto& frozen = plan.frozenSceneRootIndicesByScene[index];
				nextState.frozenRoots.reserve(frozen.size());
				for(const std::size_t rootIndex : frozen)
				{
					if(rootIndex < nextNodeIds.size())
					{
						nextState.frozenRoots.push_back(nextNodeIds[rootIndex]);
					}
				}
			}
			nextState.frozen = !nextState.frozenRoots.empty();
			const auto requestedState = std::find_if(
				plan.sceneRootStates.begin(), plan.sceneRootStates.end(),
				[&](const detail::MutationSceneRootStatePlan& value)
				{
					return value.scene == sceneId;
				});
			if(requestedState != plan.sceneRootStates.end())
			{
				const bool externalMatches = !hadOldState
					|| m_impl->sceneRootTransforms[oldIndex].externalStateSerial
						== requestedState->expectedExternalStateSerial;
				if(externalMatches || !requestedState->preserveExternalOnMismatch)
				{
					nextState.baseline.translation = requestedState->baselineTranslation;
					nextState.baseline.rotation = requestedState->baselineRotation;
					nextState.baseline.scale = requestedState->baselineScale;
					nextState.current.translation = requestedState->currentTranslation;
					nextState.current.rotation = requestedState->currentRotation;
					nextState.current.scale = requestedState->currentScale;
					if(!requestedState->frozenRootIds.empty())
					{
						nextState.frozenRoots.clear();
						nextState.frozenRoots.reserve(requestedState->frozenRootIds.size());
						for(const EditorObjectId root : requestedState->frozenRootIds)
							if(std::find(nextNodeIds.begin(), nextNodeIds.end(), root) != nextNodeIds.end())
								nextState.frozenRoots.push_back(root);
					}
					nextState.frozen = requestedState->frozen
						&& !nextState.frozenRoots.empty();
				}
				else if(hadOldState)
				{
					const auto& oldState = m_impl->sceneRootTransforms[oldIndex];
					nextState.baseline = oldState.baseline;
					nextState.current = oldState.current;
					nextState.externalStateSerial = oldState.externalStateSerial;
					nextState.frozenRoots.clear();
					for(const EditorObjectId root : oldState.frozenRoots)
						if(std::find(nextNodeIds.begin(), nextNodeIds.end(), root) != nextNodeIds.end())
							nextState.frozenRoots.push_back(root);
					nextState.frozen = oldState.frozen && !nextState.frozenRoots.empty();
				}
			}
			if(hadOldState)
			{
				if(requestedState == plan.sceneRootStates.end()
					|| !requestedState->preserveExternalOnMismatch
					|| m_impl->sceneRootTransforms[oldIndex].externalStateSerial
						== requestedState->expectedExternalStateSerial)
				{
					nextState.externalStateSerial =
						m_impl->sceneRootTransforms[oldIndex].externalStateSerial;
				}
				const auto& oldState = m_impl->sceneRootTransforms[oldIndex];
				const bool rootRuntimeChanged = oldState.baseline.translation
						!= nextState.baseline.translation
					|| oldState.baseline.rotation != nextState.baseline.rotation
					|| oldState.baseline.scale != nextState.baseline.scale
					|| oldState.current.translation != nextState.current.translation
					|| oldState.current.rotation != nextState.current.rotation
					|| oldState.current.scale != nextState.current.scale
					|| oldState.frozen != nextState.frozen
					|| oldState.frozenRoots != nextState.frozenRoots;
				std::optional<bool> sceneHierarchyChanged{false};
				if(plan.hierarchyChanged)
					sceneHierarchyChanged = occurrenceChanged(oldIndex, index);
				if(!sceneHierarchyChanged)
					return detail::MutationCommitStatus::internalError;
				if(!plan.retainExternalStateSerial
					&& (rootRuntimeChanged || *sceneHierarchyChanged))
				{
					std::uint64_t nextSerial = 0;
					if(!checkedCounterSuccessor(
						nextState.externalStateSerial, nextSerial))
						return detail::MutationCommitStatus::externalStateSerialExhausted;
					nextState.externalStateSerial = nextSerial;
				}
			}
		}

		if(prePublish != nullptr)
		{
			invokingPrePublish = true;
			prePublish(plan, prePublishContext);
			invokingPrePublish = false;
		}

		// Publication begins here. Every operation below is non-throwing.
		invalidateSemanticDigestCache();
		m_impl->model.swap(model);
		for(std::size_t kind = 0; kind < kKindCount; ++kind)
		{
			m_impl->ids[kind].idsByIndex.swap(nextIds[kind].idsByIndex);
			m_impl->ids[kind].slots.swap(nextIds[kind].slots);
		}
		m_impl->primitiveAddresses.swap(nextPrimitiveAddresses);
		m_impl->occurrenceParentsByScene.swap(nextOccurrenceParents);
		m_impl->sceneRootTransforms.swap(nextSceneRootTransforms);
		m_impl->currentScene = nextCurrent;
		m_impl->sceneActivationEpoch = nextSceneActivationEpoch;
		m_impl->revision = nextRevision;
		return detail::MutationCommitStatus::applied;
	}
	catch(const std::bad_alloc&)
	{
		return detail::MutationCommitStatus::allocationFailed;
	}
	catch(...)
	{
		return invokingPrePublish
			? detail::MutationCommitStatus::prePublishFailed
			: detail::MutationCommitStatus::internalError;
	}
}

detail::MutationCommitStatus GltfDocument::commitNodeNameMutation(
	std::size_t nodeIndex, std::string name,
	detail::MutationRegistryPlan& plan,
	detail::MutationPrePublishCallback prePublish,
	void* prePublishContext) noexcept
{
	if(!m_impl || !m_impl->model || nodeIndex >= m_impl->model->nodes.size())
		return detail::MutationCommitStatus::internalError;
	std::uint64_t nextRevision = 0;
	if(!checkedCounterSuccessor(m_impl->revision, nextRevision))
		return detail::MutationCommitStatus::revisionExhausted;

	// The specialized path is valid only for an identity-preserving plan.  The
	// checks are intentionally redundant with SceneCommands' preflight so a
	// future caller cannot accidentally bypass registry/topology validation.
	if(plan.hierarchyChanged || plan.currentScene != m_impl->currentScene
		|| plan.frozenSceneRootIndicesByScene.size() != 0u
		|| plan.sceneRootStates.size() != 0u
		|| plan.allowResurrection || plan.retainExternalStateSerial)
		return detail::MutationCommitStatus::internalError;
	for(std::size_t kind = 1; kind < kKindCount; ++kind)
	{
		const auto& requested = plan.idsByKind[kind];
		const auto& current = m_impl->ids[kind].idsByIndex;
		if(requested != current) return detail::MutationCommitStatus::internalError;
	}
	if(plan.primitiveAddresses != m_impl->primitiveAddresses)
		return detail::MutationCommitStatus::internalError;

	const std::string oldName = m_impl->model->nodes[nodeIndex].name;
	const bool cacheWasValid = m_impl->semanticDigestCacheValid;
	const SceneDigest oldDigest = m_impl->cachedDocumentDigest;
	const std::string oldSemantic = m_impl->cachedSemanticHash;
	bool invokingPrePublish = false;
	try
	{
		// std::string::swap is non-throwing.  Stage the value before invoking
		// the callback so evidence can hash the exact target authority; restore
		// it on every pre-publication failure.
		m_impl->model->nodes[nodeIndex].name.swap(name);
		invalidateSemanticDigestCache(true);
		if(!sceneDigestPresent(documentDigest()))
		{
			m_impl->model->nodes[nodeIndex].name.swap(name);
			if(cacheWasValid)
			{
				m_impl->cachedDocumentDigest = oldDigest;
				m_impl->cachedSemanticHash = oldSemantic;
				m_impl->semanticDigestCacheValid = true;
			}
			return detail::MutationCommitStatus::internalError;
		}
		if(prePublish != nullptr)
		{
			invokingPrePublish = true;
			prePublish(plan, prePublishContext);
			invokingPrePublish = false;
		}
		// Publication begins only after all potentially-throwing evidence work.
		m_impl->revision = nextRevision;
		return detail::MutationCommitStatus::applied;
	}
	catch(const std::bad_alloc&)
	{
		m_impl->model->nodes[nodeIndex].name.swap(name);
		if(cacheWasValid)
		{
			m_impl->cachedDocumentDigest = oldDigest;
			m_impl->cachedSemanticHash = oldSemantic;
			m_impl->semanticDigestCacheValid = true;
		}
		else
		{
			invalidateSemanticDigestCache();
		}
		return detail::MutationCommitStatus::allocationFailed;
	}
	catch(...)
	{
		m_impl->model->nodes[nodeIndex].name.swap(name);
		if(cacheWasValid)
		{
			m_impl->cachedDocumentDigest = oldDigest;
			m_impl->cachedSemanticHash = oldSemantic;
			m_impl->semanticDigestCacheValid = true;
		}
		else
		{
			invalidateSemanticDigestCache();
		}
		return invokingPrePublish
			? detail::MutationCommitStatus::prePublishFailed
			: detail::MutationCommitStatus::internalError;
	}
}

detail::MutationCommitStatus GltfDocument::commitMaterialPropertiesMutation(
	std::size_t materialIndex, const tinygltf::Material& record,
	detail::MutationRegistryPlan& plan,
	detail::MutationPrePublishCallback prePublish,
	void* prePublishContext) noexcept
{
	if(!m_impl || !m_impl->model ||
		materialIndex >= m_impl->model->materials.size())
		return detail::MutationCommitStatus::internalError;
	std::uint64_t nextRevision = 0;
	if(!checkedCounterSuccessor(m_impl->revision, nextRevision))
		return detail::MutationCommitStatus::revisionExhausted;

	// This path is valid only for an identity-preserving material update.
	// The caller proves that only supported material properties changed; repeat
	// the registry checks here so the in-place publication remains fail-closed.
	if(plan.hierarchyChanged || plan.currentScene != m_impl->currentScene
		|| !plan.frozenSceneRootIndicesByScene.empty()
		|| !plan.sceneRootStates.empty() || plan.allowResurrection
		|| plan.retainExternalStateSerial)
		return detail::MutationCommitStatus::internalError;
	for(std::size_t kind = 1; kind < kKindCount; ++kind)
		if(plan.idsByKind[kind] != m_impl->ids[kind].idsByIndex)
			return detail::MutationCommitStatus::internalError;
	if(plan.primitiveAddresses != m_impl->primitiveAddresses)
		return detail::MutationCommitStatus::internalError;

	auto& factors = m_impl->model->materials[materialIndex]
		.pbrMetallicRoughness.baseColorFactor;
	if(factors.size() != 4u || record.pbrMetallicRoughness.baseColorFactor.size() != 4u ||
		m_impl->model->materials[materialIndex].emissiveFactor.size() != 3u ||
		record.emissiveFactor.size() != 3u)
		return detail::MutationCommitStatus::internalError;
	const std::array<double, 4> previousBaseColor{
		factors[0], factors[1], factors[2], factors[3]};
	const std::array<double, 3> previousEmissive{
		m_impl->model->materials[materialIndex].emissiveFactor[0],
		m_impl->model->materials[materialIndex].emissiveFactor[1],
		m_impl->model->materials[materialIndex].emissiveFactor[2]};
	const double previousMetallic =
		m_impl->model->materials[materialIndex].pbrMetallicRoughness.metallicFactor;
	const double previousRoughness =
		m_impl->model->materials[materialIndex].pbrMetallicRoughness.roughnessFactor;
	const double previousAlphaCutoff =
		m_impl->model->materials[materialIndex].alphaCutoff;
	const double previousNormalScale =
		m_impl->model->materials[materialIndex].normalTexture.scale;
	const double previousOcclusionStrength =
		m_impl->model->materials[materialIndex].occlusionTexture.strength;
	std::optional<std::string> previousMaterialMetadata;
	const bool cacheWasValid = m_impl->semanticDigestCacheValid;
	const SceneDigest oldDigest = m_impl->cachedDocumentDigest;
	const std::string oldSemantic = m_impl->cachedSemanticHash;
	bool invokingPrePublish = false;
	const auto restore = [&]() noexcept {
		for(std::size_t index = 0; index < previousBaseColor.size(); ++index)
			factors[index] = previousBaseColor[index];
		for(std::size_t index = 0; index < previousEmissive.size(); ++index)
			m_impl->model->materials[materialIndex].emissiveFactor[index] =
				previousEmissive[index];
		m_impl->model->materials[materialIndex].pbrMetallicRoughness.metallicFactor = previousMetallic;
		m_impl->model->materials[materialIndex].pbrMetallicRoughness.roughnessFactor = previousRoughness;
		m_impl->model->materials[materialIndex].alphaCutoff = previousAlphaCutoff;
		m_impl->model->materials[materialIndex].normalTexture.scale = previousNormalScale;
		m_impl->model->materials[materialIndex].occlusionTexture.strength = previousOcclusionStrength;
		if(previousMaterialMetadata)
			m_impl->cachedMaterialMetadata.materials[materialIndex] = std::move(*previousMaterialMetadata);
		if(cacheWasValid)
		{
			m_impl->cachedDocumentDigest = oldDigest;
			m_impl->cachedSemanticHash = oldSemantic;
			m_impl->semanticDigestCacheValid = true;
		}
		else
		{
			invalidateSemanticDigestCache();
		}
	};
	try
	{
		std::copy(record.pbrMetallicRoughness.baseColorFactor.begin(),
			record.pbrMetallicRoughness.baseColorFactor.end(), factors.begin());
		m_impl->model->materials[materialIndex].pbrMetallicRoughness.metallicFactor =
			record.pbrMetallicRoughness.metallicFactor;
		m_impl->model->materials[materialIndex].pbrMetallicRoughness.roughnessFactor =
			record.pbrMetallicRoughness.roughnessFactor;
		std::copy(record.emissiveFactor.begin(), record.emissiveFactor.end(),
			m_impl->model->materials[materialIndex].emissiveFactor.begin());
		m_impl->model->materials[materialIndex].alphaCutoff = record.alphaCutoff;
		m_impl->model->materials[materialIndex].normalTexture.scale = record.normalTexture.scale;
		m_impl->model->materials[materialIndex].occlusionTexture.strength = record.occlusionTexture.strength;
		if(m_impl->cachedMaterialMetadata.valid &&
		   materialIndex < m_impl->cachedMaterialMetadata.materials.size())
		{
			auto serialized = detail::canonicalGltfMaterialMetadata(m_impl->model->materials[materialIndex]);
			previousMaterialMetadata = std::move(m_impl->cachedMaterialMetadata.materials[materialIndex]);
			m_impl->cachedMaterialMetadata.materials[materialIndex] = std::move(serialized);
		}
		else
		{
			m_impl->cachedMaterialMetadata = {};
		}
		invalidateSemanticDigestCache(true, true);
		if(!sceneDigestPresent(documentDigest()))
		{
			restore();
			return detail::MutationCommitStatus::internalError;
		}
		if(prePublish != nullptr)
		{
			invokingPrePublish = true;
			prePublish(plan, prePublishContext);
			invokingPrePublish = false;
		}
		m_impl->revision = nextRevision;
		return detail::MutationCommitStatus::applied;
	}
	catch(const std::bad_alloc&)
	{
		restore();
		return detail::MutationCommitStatus::allocationFailed;
	}
	catch(...)
	{
		restore();
		return invokingPrePublish
			? detail::MutationCommitStatus::prePublishFailed
			: detail::MutationCommitStatus::internalError;
	}
}

void GltfDocument::acceptSavedAuthority(
	std::unique_ptr<tinygltf::Model> model,
	std::filesystem::path sourcePath,
	GltfDocumentFormat format,
	std::vector<GltfDependency> dependencies) noexcept
{
	// The staged model proves serialized semantic equivalence, but reparsing may
	// normalize TinyGLTF's opaque JSON representation. Save As publishes only
	// persistence metadata so retained history continues to validate the exact
	// live authority records it captured.
	model.reset();
	m_impl->sourcePath.swap(sourcePath);
	m_impl->dependencies.swap(dependencies);
	m_impl->format = format;
	m_impl->cleanRevision = m_impl->revision;
}

DocumentInspection EditorStateInspector::inspect(const GltfDocument& document)
{
	DocumentInspection result;
	for(std::size_t kind = 0; kind < kKindCount; ++kind)
	{
		const auto& source = document.m_impl->ids[kind];
		result.idsByIndex[kind] = source.idsByIndex;
		result.slots[kind].reserve(source.slots.size());
		for(const auto& slot : source.slots)
		{
			result.slots[kind].push_back(ObjectSlotInspection{
				slot.generation, slot.live, slot.index});
		}
	}
	result.primitiveAddresses = document.m_impl->primitiveAddresses;
	const auto& sceneIds = document.m_impl->ids[kindIndex(EditorObjectKind::scene)].idsByIndex;
	result.sceneRoots.reserve(document.m_impl->sceneRootTransforms.size());
	for(std::size_t index = 0; index < document.m_impl->sceneRootTransforms.size(); ++index)
	{
		const auto& source = document.m_impl->sceneRootTransforms[index];
		SceneRootInspection inspection;
		if(index < sceneIds.size())
			inspection.scene = SceneId::from(sceneIds[index]).value_or(SceneId{});
		inspection.baseline = source.baseline;
		inspection.current = source.current;
		inspection.hierarchyEpoch = source.hierarchyEpoch;
		inspection.externalStateSerial = source.externalStateSerial;
		inspection.frozen = source.frozen;
		inspection.frozenRootIds.reserve(source.frozenRoots.size());
		for(const EditorObjectId id : source.frozenRoots)
			if(const auto node = NodeId::from(id)) inspection.frozenRootIds.push_back(*node);
		result.sceneRoots.push_back(std::move(inspection));
	}
	return result;
}

ReferenceRewriteProbeResult EditorStateInspector::probeReferenceRewrite(
	const GltfDocument& source,
	EditorObjectKind targetKind,
	std::span<const std::optional<std::uint32_t>> oldToNew)
{
	ReferenceRewriteProbeResult result;
	if(!isObjectKind(targetKind)) return result;
	const std::size_t count = modelObjectCount(source.authorityModel(), targetKind);
	if(oldToNew.size() != count)
	{
		result.status = ReferenceRewriteProbeStatus::invalidMap;
		return result;
	}
	std::vector<std::uint32_t> permutation;
	std::vector<bool> seen(count, false);
	try
	{
		permutation.reserve(count);
		for(const auto mapped : oldToNew)
		{
			if(!mapped || *mapped >= count || seen[*mapped])
			{
				result.status = ReferenceRewriteProbeStatus::invalidMap;
				return result;
			}
			seen[*mapped] = true;
			permutation.push_back(*mapped);
		}

		auto candidate = std::make_unique<tinygltf::Model>(source.authorityModel());
		ReferenceRewriter rewriter{*candidate, targetKind, permutation, result};
		rewriter.rewrite();
		if(!rewriter.valid)
		{
			result.status = ReferenceRewriteProbeStatus::invalidSourceReference;
			return result;
		}
		permuteTargetCollection(*candidate, targetKind, permutation);
		std::vector<GltfDependency> dependencies(
			source.dependencies().begin(), source.dependencies().end());
		auto probe = std::unique_ptr<GltfDocument>(new GltfDocument(
			std::move(candidate), source.sourcePath(), source.format(),
			std::move(dependencies)));
		if(!GltfValidator::validate(*probe).valid())
		{
			result.status = ReferenceRewriteProbeStatus::candidateValidationFailed;
			return result;
		}
		std::string error;
		const std::string semantic = probe->semanticRepresentation(error);
		if(!error.empty())
		{
			result.status = ReferenceRewriteProbeStatus::candidateValidationFailed;
			return result;
		}
		result.candidateSemanticDigest = sha256Hex(semantic);
		result.status = ReferenceRewriteProbeStatus::rewritten;
		return result;
	}
	catch(const std::bad_alloc&)
	{
		result.status = ReferenceRewriteProbeStatus::candidateValidationFailed;
		return result;
	}
	catch(...)
	{
		result.status = ReferenceRewriteProbeStatus::candidateValidationFailed;
		return result;
	}
}

#if defined(MGIF_EDITOR_TESTING)
CheckedCounterTransition EditorStateTestAccess::checkedGenerationSuccessor(
	std::uint32_t current) noexcept
{
	std::uint32_t next = 0;
	if(!checkedCounterSuccessor(current, next))
		return CheckedCounterTransition{MutationErrorCode::generationExhausted, current};
	return CheckedCounterTransition{MutationErrorCode::none, next};
}

CheckedCounterTransition EditorStateTestAccess::checkedRevisionSuccessor(
	std::uint64_t current) noexcept
{
	std::uint64_t next = 0;
	return checkedCounterSuccessor(current, next)
		? CheckedCounterTransition{MutationErrorCode::none, next}
		: CheckedCounterTransition{MutationErrorCode::revisionExhausted, current};
}

CheckedCounterTransition EditorStateTestAccess::checkedHierarchyEpochSuccessor(
	std::uint64_t current) noexcept
{
	std::uint64_t next = 0;
	return checkedCounterSuccessor(current, next)
		? CheckedCounterTransition{MutationErrorCode::none, next}
		: CheckedCounterTransition{MutationErrorCode::hierarchyEpochExhausted, current};
}

CheckedCounterTransition EditorStateTestAccess::checkedExternalSerialSuccessor(
	std::uint64_t current) noexcept
{
	std::uint64_t next = 0;
	return checkedCounterSuccessor(current, next)
		? CheckedCounterTransition{MutationErrorCode::none, next}
		: CheckedCounterTransition{MutationErrorCode::externalStateSerialExhausted, current};
}

ExhaustionSeedResult EditorStateTestAccess::seedCounterMaximum(
	GltfDocument& document, const ExhaustionSeedRequest& request) noexcept
{
	ExhaustionSeedResult result;
	result.token = MutationToken{
		document.documentAuthority(), document.revision(), 1u};
	const bool hasObject = request.object.has_value();
	const bool hasScene = request.scene.has_value();
	const bool validShape =
		(request.counter == ExhaustionCounterKind::objectGeneration && hasObject && !hasScene)
		|| (request.counter == ExhaustionCounterKind::documentRevision && !hasObject && !hasScene)
		|| ((request.counter == ExhaustionCounterKind::hierarchyEpoch
			|| request.counter == ExhaustionCounterKind::externalStateSerial)
			&& !hasObject && hasScene);
	if(!validShape) return result;

	try
	{
		if(request.counter == ExhaustionCounterKind::documentRevision)
		{
			document.m_impl->revision = std::numeric_limits<std::uint64_t>::max();
			result.status = ExhaustionSeedStatus::seeded;
			result.token.revision = document.m_impl->revision;
			return result;
		}
		if(request.counter == ExhaustionCounterKind::hierarchyEpoch
			|| request.counter == ExhaustionCounterKind::externalStateSerial)
		{
			const auto index = document.indexOf(request.scene->untyped());
			if(!index || request.scene->authority() != document.documentAuthority()
				|| *index >= document.m_impl->sceneRootTransforms.size())
			{
				result.status = ExhaustionSeedStatus::staleOrForeignTarget;
				return result;
			}
			auto& root = document.m_impl->sceneRootTransforms[*index];
			if(request.counter == ExhaustionCounterKind::hierarchyEpoch)
				root.hierarchyEpoch = std::numeric_limits<std::uint64_t>::max();
			else
				root.externalStateSerial = std::numeric_limits<std::uint64_t>::max();
			result.status = ExhaustionSeedStatus::seeded;
			return result;
		}

		const EditorObjectId object = *request.object;
		const auto index = document.indexOf(object);
		if(!index || object.authority != document.documentAuthority()
			|| !isObjectKind(object.kind))
		{
			result.status = ExhaustionSeedStatus::staleOrForeignTarget;
			return result;
		}
		auto nextRegistry = document.m_impl->ids[kindIndex(object.kind)];
		auto nextRoots = document.m_impl->sceneRootTransforms;
		SceneId nextCurrent = document.m_impl->currentScene;
		const EditorObjectId seeded{
			object.kind, object.slot, std::numeric_limits<std::uint32_t>::max(), object.authority};
		nextRegistry.idsByIndex[*index] = seeded;
		nextRegistry.slots[object.slot].generation = seeded.generation;
		if(object.kind == EditorObjectKind::node)
		{
			for(auto& root : nextRoots)
				for(EditorObjectId& id : root.frozenRoots)
					if(id == object) id = seeded;
		}
		else if(object.kind == EditorObjectKind::scene && nextCurrent.untyped() == object)
		{
			nextCurrent = *SceneId::from(seeded);
		}
		document.m_impl->ids[kindIndex(object.kind)].idsByIndex.swap(nextRegistry.idsByIndex);
		document.m_impl->ids[kindIndex(object.kind)].slots.swap(nextRegistry.slots);
		document.m_impl->sceneRootTransforms.swap(nextRoots);
		document.m_impl->currentScene = nextCurrent;
		result.status = ExhaustionSeedStatus::seeded;
		result.seededObject = seeded;
		return result;
	}
	catch(const std::bad_alloc&)
	{
		result.status = ExhaustionSeedStatus::allocationFailed;
		return result;
	}
	catch(...)
	{
		return result;
	}
}
#endif

} // namespace demo::editor
