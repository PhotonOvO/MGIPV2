#include <mgif/scene_editor/GltfValidator.h>

#include <mgif/scene_editor/GltfDocument.h>
#include <mgif/scene_editor/detail/BitwiseCast.h>

#include <tiny_gltf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace demo::editor {
namespace detail {

[[nodiscard]] bool validIndex(int index, std::size_t count, bool optional = true) noexcept
{
	return (optional && index == -1)
		|| (index >= 0 && static_cast<std::size_t>(index) < count);
}

[[nodiscard]] bool finiteValues(const std::vector<double>& values) noexcept
{
	return std::all_of(values.begin(), values.end(), [](double value) { return std::isfinite(value); });
}

[[nodiscard]] std::uint64_t accessorElementBytes(int componentBytes, int type, int components) noexcept
{
	if(componentBytes <= 0 || components <= 0)
	{
		return 0;
	}
	if(type == TINYGLTF_TYPE_MAT2 || type == TINYGLTF_TYPE_MAT3 || type == TINYGLTF_TYPE_MAT4)
	{
		const std::uint64_t columns = type == TINYGLTF_TYPE_MAT2 ? 2u : (type == TINYGLTF_TYPE_MAT3 ? 3u : 4u);
		const std::uint64_t columnBytes = columns * static_cast<std::uint64_t>(componentBytes);
		const std::uint64_t paddedColumnBytes = (columnBytes + 3u) & ~std::uint64_t{3u};
		return columns * paddedColumnBytes;
	}
	return static_cast<std::uint64_t>(componentBytes) * static_cast<std::uint64_t>(components);
}

inline constexpr std::size_t kMaxAccessorComponents = 16;

[[nodiscard]] std::uint64_t accessorComponentOffset(
	int componentBytes,
	int type,
	std::size_t component) noexcept
{
	if(type != TINYGLTF_TYPE_MAT2 && type != TINYGLTF_TYPE_MAT3 && type != TINYGLTF_TYPE_MAT4)
	{
		return static_cast<std::uint64_t>(componentBytes) * component;
	}
	const std::uint64_t dimension = type == TINYGLTF_TYPE_MAT2 ? 2u : (type == TINYGLTF_TYPE_MAT3 ? 3u : 4u);
	const std::uint64_t columnBytes = dimension * static_cast<std::uint64_t>(componentBytes);
	const std::uint64_t paddedColumnBytes = (columnBytes + 3u) & ~std::uint64_t{3u};
	const std::uint64_t column = component / dimension;
	const std::uint64_t row = component % dimension;
	return column * paddedColumnBytes + row * static_cast<std::uint64_t>(componentBytes);
}

[[nodiscard]] bool readLittleEndianComponent(
	std::span<const unsigned char> bytes,
	std::uint64_t offset,
	int componentType,
	double& value,
	std::uint64_t& unsignedValue) noexcept
{
	const int componentBytes = tinygltf::GetComponentSizeInBytes(static_cast<std::uint32_t>(componentType));
	if(componentBytes <= 0 || offset > bytes.size()
		|| static_cast<std::uint64_t>(componentBytes) > bytes.size() - offset)
	{
		return false;
	}
	const auto* source = bytes.data() + static_cast<std::size_t>(offset);
	unsignedValue = 0;
	for(int byte = 0; byte < componentBytes; ++byte)
	{
		unsignedValue |= static_cast<std::uint64_t>(source[byte]) << (byte * 8);
	}
	switch(componentType)
	{
	case TINYGLTF_COMPONENT_TYPE_BYTE:
		value = static_cast<double>(detail::bitwiseCast<std::int8_t>(
			static_cast<std::uint8_t>(unsignedValue)));
		return true;
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
		value = static_cast<double>(static_cast<std::uint8_t>(unsignedValue));
		return true;
	case TINYGLTF_COMPONENT_TYPE_SHORT:
		value = static_cast<double>(detail::bitwiseCast<std::int16_t>(
			static_cast<std::uint16_t>(unsignedValue)));
		return true;
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
		value = static_cast<double>(static_cast<std::uint16_t>(unsignedValue));
		return true;
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
		value = static_cast<double>(static_cast<std::uint32_t>(unsignedValue));
		return true;
	case TINYGLTF_COMPONENT_TYPE_FLOAT:
		value = static_cast<double>(detail::bitwiseCast<float>(
			static_cast<std::uint32_t>(unsignedValue)));
		return true;
	default:
		return false;
	}
}

[[nodiscard]] constexpr bool unsignedComponentType(int componentType) noexcept
{
	return componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE
		|| componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT
		|| componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
}

[[nodiscard]] constexpr std::uint64_t unsignedComponentMaximum(int componentType) noexcept
{
	switch(componentType)
	{
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: return std::numeric_limits<std::uint8_t>::max();
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return std::numeric_limits<std::uint16_t>::max();
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: return std::numeric_limits<std::uint32_t>::max();
	default: return 0;
	}
}

[[nodiscard]] bool accessorBoundMatches(int componentType, double declared, double actual) noexcept
{
	if(componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
	{
		return declared == actual;
	}
	const float rounded = static_cast<float>(declared);
	return std::isfinite(rounded) && static_cast<double>(rounded) == actual;
}

[[nodiscard]] bool validPrimitiveCount(int mode, std::uint64_t count) noexcept
{
	switch(mode)
	{
	case TINYGLTF_MODE_POINTS: return count != 0;
	case TINYGLTF_MODE_LINE: return count != 0 && (count % 2u) == 0;
	case TINYGLTF_MODE_LINE_LOOP:
	case TINYGLTF_MODE_LINE_STRIP: return count >= 2;
	case TINYGLTF_MODE_TRIANGLES: return count != 0 && (count % 3u) == 0;
	case TINYGLTF_MODE_TRIANGLE_STRIP:
	case TINYGLTF_MODE_TRIANGLE_FAN: return count >= 3;
	default: return false;
	}
}

enum class AccessorReadError : std::uint8_t
{
	none,
	byteRange,
	sparseIndexOutOfRange,
	sparseIndicesNotIncreasing
};

class AccessorPayloadReader final
{
public:
	AccessorPayloadReader(const tinygltf::Model& model, const tinygltf::Accessor& accessor)
		: m_model(model)
		, m_accessor(accessor)
		, m_componentBytes(tinygltf::GetComponentSizeInBytes(static_cast<std::uint32_t>(accessor.componentType)))
		, m_components(tinygltf::GetNumComponentsInType(static_cast<std::uint32_t>(accessor.type)))
		, m_elementBytes(accessorElementBytes(m_componentBytes, accessor.type, m_components))
	{
		if(accessor.bufferView >= 0)
		{
			const tinygltf::BufferView& view = m_model.bufferViews[static_cast<std::size_t>(accessor.bufferView)];
			m_stride = view.byteStride == 0 ? m_elementBytes : view.byteStride;
		}
	}

	template <typename Visitor>
	[[nodiscard]] bool scan(Visitor&& visitor)
	{
		std::array<double, kMaxAccessorComponents> values{};
		std::array<std::uint64_t, kMaxAccessorComponents> rawValues{};
		std::uint64_t sparseOrdinal = 0;
		std::uint64_t sparseIndex = 0;
		const std::uint64_t sparseCount = m_accessor.sparse.isSparse
			? static_cast<std::uint64_t>(m_accessor.sparse.count)
			: 0;
		if(sparseCount != 0 && !readSparseIndex(sparseOrdinal, sparseIndex))
		{
			return false;
		}
		if(sparseCount != 0 && sparseIndex >= m_accessor.count)
		{
			m_error = AccessorReadError::sparseIndexOutOfRange;
			return false;
		}

		for(std::uint64_t element = 0; element < m_accessor.count; ++element)
		{
			const bool sparseReplacement = sparseOrdinal < sparseCount && sparseIndex == element;
			for(std::size_t component = 0; component < static_cast<std::size_t>(m_components); ++component)
			{
				if(!readElementComponent(element, sparseReplacement, sparseOrdinal, component,
				                         values[component], rawValues[component]))
				{
					return false;
				}
			}
			if(!visitor(element,
			            std::span<const double>(values.data(), static_cast<std::size_t>(m_components)),
			            std::span<const std::uint64_t>(rawValues.data(), static_cast<std::size_t>(m_components))))
			{
				return false;
			}

			if(sparseReplacement)
			{
				const std::uint64_t previous = sparseIndex;
				++sparseOrdinal;
				if(sparseOrdinal < sparseCount)
				{
					if(!readSparseIndex(sparseOrdinal, sparseIndex))
					{
						return false;
					}
					if(sparseIndex <= previous)
					{
						m_error = AccessorReadError::sparseIndicesNotIncreasing;
						return false;
					}
					if(sparseIndex >= m_accessor.count)
					{
						m_error = AccessorReadError::sparseIndexOutOfRange;
						return false;
					}
				}
			}
		}
		return sparseOrdinal == sparseCount
			|| setError(AccessorReadError::sparseIndexOutOfRange);
	}

	[[nodiscard]] AccessorReadError error() const noexcept { return m_error; }

private:
	[[nodiscard]] bool setError(AccessorReadError error) noexcept
	{
		m_error = error;
		return false;
	}

	[[nodiscard]] bool readViewComponent(
		int viewIndex,
		std::uint64_t relativeOffset,
		int componentType,
		double& value,
		std::uint64_t& rawValue)
	{
		if(viewIndex < 0 || static_cast<std::size_t>(viewIndex) >= m_model.bufferViews.size())
		{
			return setError(AccessorReadError::byteRange);
		}
		const tinygltf::BufferView& view = m_model.bufferViews[static_cast<std::size_t>(viewIndex)];
		const int componentBytes = tinygltf::GetComponentSizeInBytes(static_cast<std::uint32_t>(componentType));
		if(componentBytes <= 0 || relativeOffset > view.byteLength
			|| static_cast<std::uint64_t>(componentBytes) > view.byteLength - relativeOffset
			|| view.buffer < 0 || static_cast<std::size_t>(view.buffer) >= m_model.buffers.size())
		{
			return setError(AccessorReadError::byteRange);
		}
		std::uint64_t absoluteOffset = 0;
		if(!editorCheckedAdd(view.byteOffset, relativeOffset, absoluteOffset))
		{
			return setError(AccessorReadError::byteRange);
		}
		const auto& data = m_model.buffers[static_cast<std::size_t>(view.buffer)].data;
		return readLittleEndianComponent(std::span<const unsigned char>(data.data(), data.size()),
		                                 absoluteOffset,
		                                 componentType,
		                                 value,
		                                 rawValue)
			|| setError(AccessorReadError::byteRange);
	}

	[[nodiscard]] bool readSparseIndex(std::uint64_t ordinal, std::uint64_t& value)
	{
		const int componentType = m_accessor.sparse.indices.componentType;
		const int bytes = tinygltf::GetComponentSizeInBytes(static_cast<std::uint32_t>(componentType));
		std::uint64_t ordinalOffset = 0;
		std::uint64_t relativeOffset = 0;
		double unused = 0.0;
		if(bytes > 0
			&& editorCheckedMultiply(ordinal, static_cast<std::uint64_t>(bytes), ordinalOffset)
			&& editorCheckedAdd(m_accessor.sparse.indices.byteOffset, ordinalOffset, relativeOffset)
			&& readViewComponent(m_accessor.sparse.indices.bufferView, relativeOffset, componentType, unused, value))
		{
			return true;
		}
		return setError(AccessorReadError::byteRange);
	}

	[[nodiscard]] bool readElementComponent(
		std::uint64_t element,
		bool sparseReplacement,
		std::uint64_t sparseOrdinal,
		std::size_t component,
		double& value,
		std::uint64_t& rawValue)
	{
		const std::uint64_t componentOffset = accessorComponentOffset(m_componentBytes, m_accessor.type, component);
		if(sparseReplacement)
		{
			std::uint64_t elementOffset = 0;
			std::uint64_t relativeOffset = 0;
			if(editorCheckedMultiply(sparseOrdinal, m_elementBytes, elementOffset)
				&& editorCheckedAdd(m_accessor.sparse.values.byteOffset, elementOffset, relativeOffset)
				&& editorCheckedAdd(relativeOffset, componentOffset, relativeOffset)
				&& readViewComponent(m_accessor.sparse.values.bufferView,
				                     relativeOffset,
				                     m_accessor.componentType,
				                     value,
				                     rawValue))
			{
				return true;
			}
			return setError(AccessorReadError::byteRange);
		}
		if(m_accessor.bufferView < 0)
		{
			value = 0.0;
			rawValue = 0;
			return true;
		}
		std::uint64_t elementOffset = 0;
		std::uint64_t relativeOffset = 0;
		if(editorCheckedMultiply(element, m_stride, elementOffset)
			&& editorCheckedAdd(m_accessor.byteOffset, elementOffset, relativeOffset)
			&& editorCheckedAdd(relativeOffset, componentOffset, relativeOffset)
			&& readViewComponent(m_accessor.bufferView,
			                     relativeOffset,
			                     m_accessor.componentType,
			                     value,
			                     rawValue))
		{
			return true;
		}
		return setError(AccessorReadError::byteRange);
	}

	const tinygltf::Model& m_model;
	const tinygltf::Accessor& m_accessor;
	int m_componentBytes{0};
	int m_components{0};
	std::uint64_t m_elementBytes{0};
	std::uint64_t m_stride{0};
	AccessorReadError m_error{AccessorReadError::none};
};

struct AccessorRequirements final
{
	bool primitiveIndices{false};
	bool animationInput{false};
	bool weights{false};
	bool joints{false};
	std::uint64_t primitiveIndexUpperBound{std::numeric_limits<std::uint64_t>::max()};
	std::uint64_t jointUpperBound{std::numeric_limits<std::uint64_t>::max()};
};

class GltfModelValidator final
{
public:
	GltfModelValidator(const tinygltf::Model& model, const EditorLimits& limits)
		: m_model(model), m_limits(limits)
	{
	}

	[[nodiscard]] GltfValidationReport run()
	{
		if(!validateCounts()
			|| !validateStandardStringsAndLegacyValues()
			|| !validateExtensionDeclarations()
			|| !validateOpaqueValues()
			|| !validateBuffersAndAccessors()
			|| !validateImagesAndTextures()
			|| !validateMaterialsAndMeshes()
			|| !validateNodesAndScenes()
			|| !validateSkinsAndAnimations()
			|| !validateCamerasLightsAndAudio()
			|| !validateKnownExtensionReferences())
		{
			return std::move(m_report);
		}
		return std::move(m_report);
	}

private:
	[[nodiscard]] bool fail(GltfValidationErrorCode code, std::string path, std::string message)
	{
		m_report.m_valid = false;
		if(m_report.m_issues.size() < m_limits.maxValidationIssues)
		{
			m_report.m_issues.push_back(GltfValidationIssue{code, std::move(path), std::move(message)});
		}
		return false;
	}

	[[nodiscard]] bool checkCount(std::string_view path, std::size_t actual, std::size_t maximum)
	{
		if(actual <= maximum && actual <= std::numeric_limits<std::uint32_t>::max())
		{
			return true;
		}
		return fail(
			GltfValidationErrorCode::limitExceeded,
			std::string(path),
			"collection contains " + std::to_string(actual)
				+ " entries; limit is " + std::to_string(maximum));
	}

	[[nodiscard]] bool checkReference(
		int index,
		std::size_t count,
		std::string path,
		bool optional = true)
	{
		if(validIndex(index, count, optional))
		{
			return true;
		}
		return fail(
			GltfValidationErrorCode::indexOutOfRange,
			std::move(path),
			"index " + std::to_string(index) + " is outside collection size " + std::to_string(count));
	}

	[[nodiscard]] bool checkString(std::string_view value, std::string path)
	{
		return value.size() <= m_limits.maxStringBytes
			|| fail(GltfValidationErrorCode::limitExceeded,
			        std::move(path),
			        "string exceeds the configured byte limit");
	}

	[[nodiscard]] bool validateValue(const tinygltf::Value& root, std::string path)
	{
		struct PendingValue final
		{
			const tinygltf::Value* value;
			std::size_t depth;
		};
		std::vector<PendingValue> pending{{&root, 1}};
		while(!pending.empty())
		{
			const PendingValue current = pending.back();
			pending.pop_back();
			if(current.depth > m_limits.maxJsonDepth
				|| m_jsonValueCount >= m_limits.maxJsonValues)
			{
				return fail(GltfValidationErrorCode::limitExceeded,
				            path,
				            "extension/extras JSON depth or value count exceeds its limit");
			}
			++m_jsonValueCount;

			if(current.value->IsNumber())
			{
				if(!std::isfinite(current.value->GetNumberAsDouble()))
				{
					return fail(GltfValidationErrorCode::invalidValue, path, "extension/extras contains a non-finite number");
				}
			}
			else if(current.value->IsString())
			{
				if(!checkString(current.value->Get<std::string>(), path))
				{
					return false;
				}
			}
			else if(current.value->IsBinary())
			{
				if(current.value->Get<std::vector<unsigned char>>().size() > m_limits.maxAuthorityPayloadBytes)
				{
					return fail(GltfValidationErrorCode::limitExceeded, path, "extension binary payload exceeds its limit");
				}
			}
			else if(current.value->IsArray())
			{
				const auto& values = current.value->Get<tinygltf::Value::Array>();
				if(values.size() > m_limits.maxJsonValues - std::min(m_jsonValueCount, m_limits.maxJsonValues))
				{
					return fail(GltfValidationErrorCode::limitExceeded, path, "extension/extras array exceeds its value limit");
				}
				for(const tinygltf::Value& value : values)
				{
					pending.push_back(PendingValue{&value, current.depth + 1});
				}
			}
			else if(current.value->IsObject())
			{
				const auto& values = current.value->Get<tinygltf::Value::Object>();
				if(values.size() > m_limits.maxJsonValues - std::min(m_jsonValueCount, m_limits.maxJsonValues))
				{
					return fail(GltfValidationErrorCode::limitExceeded, path, "extension/extras object exceeds its value limit");
				}
				for(const auto& [key, value] : values)
				{
					if(!checkString(key, path))
					{
						return false;
					}
					pending.push_back(PendingValue{&value, current.depth + 1});
				}
			}
			else if(!current.value->IsBool() && current.value->Type() != tinygltf::NULL_TYPE)
			{
				return fail(GltfValidationErrorCode::invalidValue, path, "extension/extras contains an unknown value type");
			}
		}
		return true;
	}

	[[nodiscard]] bool validateOpaque(
		const tinygltf::ExtensionMap& extensions,
		const tinygltf::Value& extras,
		const std::string& path)
	{
		for(const auto& [name, value] : extensions)
		{
			if(name.empty() || !m_declaredExtensions.contains(name)
				|| !checkString(name, path + ".extensions")
				|| !validateValue(value, path + ".extensions." + name))
			{
				return name.empty() || !m_declaredExtensions.contains(name)
					? fail(GltfValidationErrorCode::invalidExtensionDeclaration,
					       path + ".extensions",
					       "extension name is empty or absent from extensionsUsed")
					: false;
			}
		}
		return validateValue(extras, path + ".extras");
	}

	[[nodiscard]] bool validateOpaqueValues()
	{
		const auto object = [this](const auto& value, const std::string& path)
		{
			return validateOpaque(value.extensions, value.extras, path);
		};
		if(!validateOpaque(m_model.extensions, m_model.extras, "model")
			|| !object(m_model.asset, "asset"))
		{
			return false;
		}
		for(std::size_t i = 0; i < m_model.accessors.size(); ++i)
		{
			const auto& value = m_model.accessors[i];
			const std::string path = "accessors[" + std::to_string(i) + "]";
			if(!object(value, path)
				|| !object(value.sparse, path + ".sparse")
				|| !object(value.sparse.indices, path + ".sparse.indices")
				|| !object(value.sparse.values, path + ".sparse.values")) return false;
		}
		for(std::size_t i = 0; i < m_model.animations.size(); ++i)
		{
			const auto& animation = m_model.animations[i];
			const std::string path = "animations[" + std::to_string(i) + "]";
			if(!object(animation, path)) return false;
			for(std::size_t j = 0; j < animation.samplers.size(); ++j)
				if(!object(animation.samplers[j], path + ".samplers[" + std::to_string(j) + "]")) return false;
			for(std::size_t j = 0; j < animation.channels.size(); ++j)
			{
				const auto& channel = animation.channels[j];
				const std::string channelPath = path + ".channels[" + std::to_string(j) + "]";
				if(!object(channel, channelPath)
					|| !validateOpaque(channel.target_extensions, channel.target_extras, channelPath + ".target")) return false;
			}
		}
		for(std::size_t i = 0; i < m_model.materials.size(); ++i)
		{
			const auto& material = m_model.materials[i];
			const std::string path = "materials[" + std::to_string(i) + "]";
			if(!object(material, path)
				|| !object(material.pbrMetallicRoughness, path + ".pbrMetallicRoughness")
				|| !object(material.pbrMetallicRoughness.baseColorTexture, path + ".pbrMetallicRoughness.baseColorTexture")
				|| !object(material.pbrMetallicRoughness.metallicRoughnessTexture, path + ".pbrMetallicRoughness.metallicRoughnessTexture")
				|| !object(material.normalTexture, path + ".normalTexture")
				|| !object(material.occlusionTexture, path + ".occlusionTexture")
				|| !object(material.emissiveTexture, path + ".emissiveTexture")) return false;
		}
		for(std::size_t i = 0; i < m_model.meshes.size(); ++i)
		{
			const auto& mesh = m_model.meshes[i];
			const std::string path = "meshes[" + std::to_string(i) + "]";
			if(!object(mesh, path)) return false;
			for(std::size_t j = 0; j < mesh.primitives.size(); ++j)
				if(!object(mesh.primitives[j], path + ".primitives[" + std::to_string(j) + "]")) return false;
		}
		const auto visitCollection = [&object](const auto& values, std::string_view name)
		{
			for(std::size_t i = 0; i < values.size(); ++i)
				if(!object(values[i], std::string(name) + "[" + std::to_string(i) + "]")) return false;
			return true;
		};
		if(!visitCollection(m_model.buffers, "buffers")
			|| !visitCollection(m_model.bufferViews, "bufferViews")
			|| !visitCollection(m_model.nodes, "nodes")
			|| !visitCollection(m_model.textures, "textures")
			|| !visitCollection(m_model.images, "images")
			|| !visitCollection(m_model.skins, "skins")
			|| !visitCollection(m_model.samplers, "samplers")
			|| !visitCollection(m_model.scenes, "scenes")
			|| !visitCollection(m_model.audioSources, "audioSources")) return false;
		for(std::size_t i = 0; i < m_model.cameras.size(); ++i)
		{
			const auto& camera = m_model.cameras[i];
			const std::string path = "cameras[" + std::to_string(i) + "]";
			if(!object(camera, path)
				|| !object(camera.perspective, path + ".perspective")
				|| !object(camera.orthographic, path + ".orthographic")) return false;
		}
		for(std::size_t i = 0; i < m_model.lights.size(); ++i)
		{
			const auto& light = m_model.lights[i];
			const std::string path = "lights[" + std::to_string(i) + "]";
			if(!object(light, path) || !object(light.spot, path + ".spot")) return false;
		}
		for(std::size_t i = 0; i < m_model.audioEmitters.size(); ++i)
		{
			const auto& emitter = m_model.audioEmitters[i];
			const std::string path = "audioEmitters[" + std::to_string(i) + "]";
			if(!object(emitter, path) || !object(emitter.positional, path + ".positional")) return false;
		}
		return true;
	}

	[[nodiscard]] bool validateCounts()
	{
		if(!checkCount("scenes", m_model.scenes.size(), m_limits.maxScenes)
			|| !checkCount("nodes", m_model.nodes.size(), m_limits.maxNodes)
			|| !checkCount("meshes", m_model.meshes.size(), m_limits.maxMeshes)
			|| !checkCount("materials", m_model.materials.size(), m_limits.maxMaterials)
			|| !checkCount("textures", m_model.textures.size(), m_limits.maxTextures)
			|| !checkCount("images", m_model.images.size(), m_limits.maxImages)
			|| !checkCount("samplers", m_model.samplers.size(), m_limits.maxSamplers)
			|| !checkCount("cameras", m_model.cameras.size(), m_limits.maxCameras)
			|| !checkCount("lights", m_model.lights.size(), m_limits.maxLights)
			|| !checkCount("accessors", m_model.accessors.size(), m_limits.maxAccessors)
			|| !checkCount("bufferViews", m_model.bufferViews.size(), m_limits.maxBufferViews)
			|| !checkCount("buffers", m_model.buffers.size(), m_limits.maxBuffers)
			|| !checkCount("skins", m_model.skins.size(), m_limits.maxSkins)
			|| !checkCount("animations", m_model.animations.size(), m_limits.maxAnimations)
			|| !checkCount("audioEmitters", m_model.audioEmitters.size(), m_limits.maxAudioEmitters)
			|| !checkCount("audioSources", m_model.audioSources.size(), m_limits.maxAudioSources))
		{
			return false;
		}

		std::uint64_t primitiveCount = 0;
		std::uint64_t morphTargetCount = 0;
		for(const tinygltf::Mesh& mesh : m_model.meshes)
		{
			if(!editorCheckedAdd(primitiveCount, static_cast<std::uint64_t>(mesh.primitives.size()), primitiveCount))
			{
				return fail(GltfValidationErrorCode::limitExceeded, "meshes.primitives", "primitive count overflows");
			}
			for(const tinygltf::Primitive& primitive : mesh.primitives)
			{
				if(!editorCheckedAdd(morphTargetCount, primitive.targets.size(), morphTargetCount))
				{
					return fail(GltfValidationErrorCode::limitExceeded, "meshes.primitives.targets", "morph target count overflows");
				}
			}
		}
		std::uint64_t animationChannels = 0;
		for(const tinygltf::Animation& animation : m_model.animations)
		{
			if(!editorCheckedAdd(animationChannels, animation.channels.size(), animationChannels)
				|| !editorCheckedAdd(animationChannels, animation.samplers.size(), animationChannels))
			{
				return fail(GltfValidationErrorCode::limitExceeded, "animations", "animation work count overflows");
			}
		}
		std::uint64_t skinJoints = 0;
		for(const tinygltf::Skin& skin : m_model.skins)
		{
			if(!editorCheckedAdd(skinJoints, skin.joints.size(), skinJoints))
			{
				return fail(GltfValidationErrorCode::limitExceeded, "skins.joints", "skin joint count overflows");
			}
		}
		const std::uint64_t idCapacity = std::numeric_limits<std::uint32_t>::max();
		return ((primitiveCount <= m_limits.maxPrimitives && primitiveCount <= idCapacity)
			&& (morphTargetCount <= m_limits.maxMorphTargets)
			&& (animationChannels <= m_limits.maxAnimationChannels)
			&& (skinJoints <= m_limits.maxSkinJoints))
			|| fail(GltfValidationErrorCode::limitExceeded,
			        "collections",
			        "primitive, morph-target, animation, skin-joint, or stable-ID capacity exceeds its limit");
	}

	[[nodiscard]] bool validateParameterMap(
		const tinygltf::ParameterMap& parameters,
		const std::string& path)
	{
		for(const auto& [name, parameter] : parameters)
		{
			if(!checkString(name, path) || !checkString(parameter.string_value, path + "." + name)
				|| !std::isfinite(parameter.number_value) || !finiteValues(parameter.number_array))
			{
				return m_report.valid()
					? fail(GltfValidationErrorCode::invalidValue,
					       path + "." + name,
					       "legacy material parameter contains a non-finite number")
					: false;
			}
			for(const auto& [member, value] : parameter.json_double_value)
			{
				if(!checkString(member, path + "." + name) || !std::isfinite(value))
				{
					return m_report.valid()
						? fail(GltfValidationErrorCode::invalidValue,
						       path + "." + name + "." + member,
						       "legacy material parameter contains a non-finite number")
						: false;
				}
			}
		}
		return true;
	}

	[[nodiscard]] bool validateStandardStringsAndLegacyValues()
	{
		if(!checkString(m_model.asset.version, "asset.version")
			|| !checkString(m_model.asset.minVersion, "asset.minVersion")
			|| !checkString(m_model.asset.generator, "asset.generator")
			|| !checkString(m_model.asset.copyright, "asset.copyright"))
		{
			return false;
		}
		for(std::size_t index = 0; index < m_model.extensionsUsed.size(); ++index)
		{
			if(!checkString(m_model.extensionsUsed[index], "extensionsUsed[" + std::to_string(index) + "]")) return false;
		}
		for(std::size_t index = 0; index < m_model.extensionsRequired.size(); ++index)
		{
			if(!checkString(m_model.extensionsRequired[index], "extensionsRequired[" + std::to_string(index) + "]")) return false;
		}

		const auto checkNames = [this](const auto& values, std::string_view collection)
		{
			for(std::size_t index = 0; index < values.size(); ++index)
			{
				if(!checkString(values[index].name,
				                std::string(collection) + "[" + std::to_string(index) + "].name")) return false;
			}
			return true;
		};
		if(!checkNames(m_model.accessors, "accessors")
			|| !checkNames(m_model.animations, "animations")
			|| !checkNames(m_model.buffers, "buffers")
			|| !checkNames(m_model.bufferViews, "bufferViews")
			|| !checkNames(m_model.materials, "materials")
			|| !checkNames(m_model.meshes, "meshes")
			|| !checkNames(m_model.nodes, "nodes")
			|| !checkNames(m_model.textures, "textures")
			|| !checkNames(m_model.images, "images")
			|| !checkNames(m_model.skins, "skins")
			|| !checkNames(m_model.samplers, "samplers")
			|| !checkNames(m_model.cameras, "cameras")
			|| !checkNames(m_model.scenes, "scenes")
			|| !checkNames(m_model.lights, "lights")
			|| !checkNames(m_model.audioEmitters, "audioEmitters")
			|| !checkNames(m_model.audioSources, "audioSources"))
		{
			return false;
		}

		for(std::size_t index = 0; index < m_model.buffers.size(); ++index)
		{
			if(!checkString(m_model.buffers[index].uri, "buffers[" + std::to_string(index) + "].uri")) return false;
		}
		for(std::size_t index = 0; index < m_model.images.size(); ++index)
		{
			if(!checkString(m_model.images[index].uri, "images[" + std::to_string(index) + "].uri")
				|| !checkString(m_model.images[index].mimeType, "images[" + std::to_string(index) + "].mimeType")) return false;
		}
		for(std::size_t index = 0; index < m_model.materials.size(); ++index)
		{
			const auto& material = m_model.materials[index];
			const std::string base = "materials[" + std::to_string(index) + "]";
			if(!checkString(material.alphaMode, base + ".alphaMode")
				|| !validateParameterMap(material.values, base + ".values")
				|| !validateParameterMap(material.additionalValues, base + ".additionalValues")) return false;
		}
		for(std::size_t meshIndex = 0; meshIndex < m_model.meshes.size(); ++meshIndex)
		{
			for(std::size_t primitiveIndex = 0;
				primitiveIndex < m_model.meshes[meshIndex].primitives.size();
				++primitiveIndex)
			{
				const auto& primitive = m_model.meshes[meshIndex].primitives[primitiveIndex];
				const std::string base = "meshes[" + std::to_string(meshIndex) + "].primitives["
					+ std::to_string(primitiveIndex) + "]";
				for(const auto& [semantic, unused] : primitive.attributes)
				{
					(void)unused;
					if(!checkString(semantic, base + ".attributes")) return false;
				}
				for(std::size_t targetIndex = 0; targetIndex < primitive.targets.size(); ++targetIndex)
				{
					for(const auto& [semantic, unused] : primitive.targets[targetIndex])
					{
						(void)unused;
						if(!checkString(semantic, base + ".targets[" + std::to_string(targetIndex) + "]")) return false;
					}
				}
			}
		}
		for(std::size_t animationIndex = 0; animationIndex < m_model.animations.size(); ++animationIndex)
		{
			const auto& animation = m_model.animations[animationIndex];
			for(std::size_t samplerIndex = 0; samplerIndex < animation.samplers.size(); ++samplerIndex)
			{
				if(!checkString(animation.samplers[samplerIndex].interpolation,
				                "animations[" + std::to_string(animationIndex) + "].samplers["
						+ std::to_string(samplerIndex) + "].interpolation")) return false;
			}
			for(std::size_t channelIndex = 0; channelIndex < animation.channels.size(); ++channelIndex)
			{
				if(!checkString(animation.channels[channelIndex].target_path,
				                "animations[" + std::to_string(animationIndex) + "].channels["
						+ std::to_string(channelIndex) + "].target.path")) return false;
			}
		}
		for(std::size_t index = 0; index < m_model.cameras.size(); ++index)
		{
			if(!checkString(m_model.cameras[index].type, "cameras[" + std::to_string(index) + "].type")) return false;
		}
		for(std::size_t index = 0; index < m_model.lights.size(); ++index)
		{
			if(!checkString(m_model.lights[index].type, "lights[" + std::to_string(index) + "].type")) return false;
		}
		for(std::size_t index = 0; index < m_model.audioSources.size(); ++index)
		{
			const auto& source = m_model.audioSources[index];
			if(!checkString(source.uri, "audioSources[" + std::to_string(index) + "].uri")
				|| !checkString(source.mimeType, "audioSources[" + std::to_string(index) + "].mimeType")) return false;
		}
		for(std::size_t index = 0; index < m_model.audioEmitters.size(); ++index)
		{
			const auto& emitter = m_model.audioEmitters[index];
			if(!checkString(emitter.type, "audioEmitters[" + std::to_string(index) + "].type")
				|| !checkString(emitter.distanceModel, "audioEmitters[" + std::to_string(index) + "].distanceModel")) return false;
		}
		return true;
	}

	[[nodiscard]] bool validateExtensionDeclarations()
	{
		std::unordered_set<std::string> used;
		used.reserve(m_model.extensionsUsed.size());
		for(std::size_t index = 0; index < m_model.extensionsUsed.size(); ++index)
		{
			const std::string& name = m_model.extensionsUsed[index];
			if(name.empty() || !used.insert(name).second)
			{
				return fail(GltfValidationErrorCode::invalidExtensionDeclaration,
				            "extensionsUsed[" + std::to_string(index) + "]",
				            "extension names must be non-empty and unique");
			}
		}
		m_declaredExtensions = used;

		std::unordered_set<std::string> required;
		required.reserve(m_model.extensionsRequired.size());
		for(std::size_t index = 0; index < m_model.extensionsRequired.size(); ++index)
		{
			const std::string& name = m_model.extensionsRequired[index];
			if(name.empty() || !required.insert(name).second || !used.contains(name))
			{
				return fail(GltfValidationErrorCode::invalidExtensionDeclaration,
				            "extensionsRequired[" + std::to_string(index) + "]",
				            "required extensions must be unique members of extensionsUsed");
			}
		}
		return true;
	}

	[[nodiscard]] bool validateBuffersAndAccessors()
	{
		std::uint64_t authorityBytes = 0;
		for(std::size_t index = 0; index < m_model.buffers.size(); ++index)
		{
			const std::uint64_t bytes = static_cast<std::uint64_t>(m_model.buffers[index].data.size());
			if(bytes == 0 || bytes > m_limits.maxBufferBytes)
			{
				return fail(GltfValidationErrorCode::limitExceeded,
				            "buffers[" + std::to_string(index) + "].data",
				            "buffer payload is empty or exceeds its configured byte limit");
			}
			if(!editorCheckedAdd(authorityBytes, bytes, authorityBytes)
				|| authorityBytes > m_limits.maxAuthorityPayloadBytes)
			{
				return fail(GltfValidationErrorCode::limitExceeded,
				            "buffers",
				            "aggregate authority payload exceeds configured byte limit");
			}
		}

		for(std::size_t index = 0; index < m_model.bufferViews.size(); ++index)
		{
			const tinygltf::BufferView& view = m_model.bufferViews[index];
			const std::string base = "bufferViews[" + std::to_string(index) + "]";
			if(!checkReference(view.buffer, m_model.buffers.size(), base + ".buffer", false))
			{
				return false;
			}
			if(view.byteLength == 0)
			{
				return fail(GltfValidationErrorCode::invalidBufferRange, base + ".byteLength", "bufferView must be non-empty");
			}
			std::uint64_t end = 0;
			if(!editorCheckedAdd(view.byteOffset, view.byteLength, end)
				|| end > m_model.buffers[static_cast<std::size_t>(view.buffer)].data.size())
			{
				return fail(GltfValidationErrorCode::invalidBufferRange, base, "bufferView range exceeds its buffer");
			}
			if(view.byteStride != 0
				&& (view.byteStride < 4 || view.byteStride > 252 || (view.byteStride % 4) != 0))
			{
				return fail(GltfValidationErrorCode::invalidBufferRange, base + ".byteStride", "byteStride is outside glTF bounds");
			}
			if(view.target != 0
				&& view.target != TINYGLTF_TARGET_ARRAY_BUFFER
				&& view.target != TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER)
			{
				return fail(GltfValidationErrorCode::invalidValue, base + ".target", "bufferView target is invalid");
			}
		}

		std::uint64_t decodeWorkBytes = 0;
		for(std::size_t index = 0; index < m_model.accessors.size(); ++index)
		{
			if(!validateAccessorMetadata(index, decodeWorkBytes)) return false;
		}
		collectAccessorRequirements();
		for(std::size_t index = 0; index < m_model.accessors.size(); ++index)
		{
			if(!validateAccessorPayload(index)) return false;
		}
		return true;
	}

	[[nodiscard]] bool validateAccessorMetadata(std::size_t index, std::uint64_t& decodeWorkBytes)
	{
		const tinygltf::Accessor& accessor = m_model.accessors[index];
		const std::string base = "accessors[" + std::to_string(index) + "]";
		if(accessor.count == 0 || accessor.count > m_limits.maxAccessorElements)
		{
			return fail(GltfValidationErrorCode::limitExceeded, base + ".count", "accessor count is zero or exceeds its limit");
		}
		const int componentBytes = tinygltf::GetComponentSizeInBytes(static_cast<std::uint32_t>(accessor.componentType));
		const int components = tinygltf::GetNumComponentsInType(static_cast<std::uint32_t>(accessor.type));
		const bool validComponentType = accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE
			|| accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE
			|| accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT
			|| accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT
			|| accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT
			|| accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT;
		if(!validComponentType || componentBytes <= 0 || components <= 0)
		{
			return fail(GltfValidationErrorCode::invalidAccessor, base, "accessor componentType or type is invalid");
		}
		const bool normalizedType = accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE
			|| accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE
			|| accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT
			|| accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
		if(accessor.normalized && !normalizedType)
		{
			return fail(GltfValidationErrorCode::invalidAccessor,
			            base + ".normalized",
			            "only 8-bit and 16-bit integer accessors may be normalized");
		}
		if((!accessor.minValues.empty() && accessor.minValues.size() != static_cast<std::size_t>(components))
			|| (!accessor.maxValues.empty() && accessor.maxValues.size() != static_cast<std::size_t>(components))
			|| !finiteValues(accessor.minValues) || !finiteValues(accessor.maxValues))
		{
			return fail(GltfValidationErrorCode::invalidAccessor, base, "accessor min/max shape or value is invalid");
		}
		if(!accessor.minValues.empty() && !accessor.maxValues.empty())
		{
			for(std::size_t component = 0; component < accessor.minValues.size(); ++component)
			{
				if(accessor.minValues[component] > accessor.maxValues[component])
				{
					return fail(GltfValidationErrorCode::invalidAccessor, base, "accessor minimum exceeds maximum");
				}
			}
		}

		if(!checkReference(accessor.bufferView, m_model.bufferViews.size(), base + ".bufferView"))
		{
			return false;
		}
		const std::uint64_t elementBytes = accessorElementBytes(componentBytes, accessor.type, components);
		if(elementBytes == 0)
		{
			return fail(GltfValidationErrorCode::invalidAccessor, base, "accessor element size overflows");
		}
		if(accessor.bufferView >= 0)
		{
			const tinygltf::BufferView& view = m_model.bufferViews[static_cast<std::size_t>(accessor.bufferView)];
			const std::uint64_t stride = view.byteStride == 0 ? elementBytes : view.byteStride;
			std::uint64_t absoluteOffset = 0;
			if(!editorCheckedAdd(view.byteOffset, accessor.byteOffset, absoluteOffset)
				|| (absoluteOffset % static_cast<std::uint64_t>(componentBytes)) != 0
				|| stride < elementBytes || (stride % static_cast<std::uint64_t>(componentBytes)) != 0)
			{
				return fail(GltfValidationErrorCode::invalidAccessor, base, "accessor stride is incompatible with its element type");
			}
			std::uint64_t lastOffset = 0;
			std::uint64_t rangeEnd = 0;
			if(!editorCheckedMultiply(static_cast<std::uint64_t>(accessor.count - 1), stride, lastOffset)
				|| !editorCheckedAdd(accessor.byteOffset, lastOffset, rangeEnd)
				|| !editorCheckedAdd(rangeEnd, elementBytes, rangeEnd)
				|| rangeEnd > view.byteLength)
			{
				return fail(GltfValidationErrorCode::invalidAccessor, base, "accessor range exceeds its bufferView");
			}
		}
		else if(!accessor.sparse.isSparse)
		{
			return fail(GltfValidationErrorCode::invalidAccessor,
			            base + ".bufferView",
			            "accessor without a bufferView must provide sparse data");
		}

		if(accessor.sparse.isSparse)
		{
			if(accessor.sparse.count <= 0
				|| static_cast<std::size_t>(accessor.sparse.count) > accessor.count
				|| !checkReference(accessor.sparse.indices.bufferView,
				                   m_model.bufferViews.size(),
				                   base + ".sparse.indices.bufferView",
				                   false)
				|| !checkReference(accessor.sparse.values.bufferView,
				                   m_model.bufferViews.size(),
				                   base + ".sparse.values.bufferView",
				                   false))
			{
				return m_report.valid()
					? fail(GltfValidationErrorCode::invalidAccessor, base + ".sparse", "sparse accessor metadata is invalid")
					: false;
			}
			const int sparseType = accessor.sparse.indices.componentType;
			if(sparseType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE
				&& sparseType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT
				&& sparseType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
			{
				return fail(GltfValidationErrorCode::invalidAccessor,
				            base + ".sparse.indices.componentType",
				            "sparse index component type is invalid");
			}
			const int sparseIndexBytes = tinygltf::GetComponentSizeInBytes(static_cast<std::uint32_t>(sparseType));
			const tinygltf::BufferView& indexView =
				m_model.bufferViews[static_cast<std::size_t>(accessor.sparse.indices.bufferView)];
			const tinygltf::BufferView& valueView =
				m_model.bufferViews[static_cast<std::size_t>(accessor.sparse.values.bufferView)];
			if(indexView.byteStride != 0 || valueView.byteStride != 0)
			{
				return fail(GltfValidationErrorCode::invalidAccessor,
				            base + ".sparse",
				            "sparse index and value bufferViews must be tightly packed");
			}
			std::uint64_t sparseIndexBytesTotal = 0;
			std::uint64_t sparseValueBytesTotal = 0;
			std::uint64_t sparseIndexEnd = 0;
			std::uint64_t sparseValueEnd = 0;
			std::uint64_t sparseIndexAbsoluteOffset = 0;
			std::uint64_t sparseValueAbsoluteOffset = 0;
			if(sparseIndexBytes <= 0
				|| !editorCheckedAdd(indexView.byteOffset,
				                     accessor.sparse.indices.byteOffset,
				                     sparseIndexAbsoluteOffset)
				|| (sparseIndexAbsoluteOffset % static_cast<std::uint64_t>(sparseIndexBytes)) != 0
				|| !editorCheckedAdd(valueView.byteOffset,
				                     accessor.sparse.values.byteOffset,
				                     sparseValueAbsoluteOffset)
				|| (sparseValueAbsoluteOffset % static_cast<std::uint64_t>(componentBytes)) != 0
				|| !editorCheckedMultiply(accessor.sparse.count, sparseIndexBytes, sparseIndexBytesTotal)
				|| !editorCheckedAdd(accessor.sparse.indices.byteOffset, sparseIndexBytesTotal, sparseIndexEnd)
				|| sparseIndexEnd > indexView.byteLength
				|| !editorCheckedMultiply(accessor.sparse.count, elementBytes, sparseValueBytesTotal)
				|| !editorCheckedAdd(accessor.sparse.values.byteOffset, sparseValueBytesTotal, sparseValueEnd)
				|| sparseValueEnd > valueView.byteLength)
			{
				return fail(GltfValidationErrorCode::invalidAccessor,
				            base + ".sparse",
				            "sparse accessor indices or values exceed their bufferView");
			}
		}

		std::uint64_t logicalBytes = 0;
		if(!editorCheckedMultiply(accessor.count, elementBytes, logicalBytes)
			|| !editorCheckedAdd(decodeWorkBytes, logicalBytes, decodeWorkBytes))
		{
			return fail(GltfValidationErrorCode::limitExceeded, base, "accessor decode work overflows");
		}
		if(accessor.sparse.isSparse)
		{
			const int sparseIndexBytes = tinygltf::GetComponentSizeInBytes(
				static_cast<std::uint32_t>(accessor.sparse.indices.componentType));
			std::uint64_t sparseWork = 0;
			if(sparseIndexBytes <= 0
				|| !editorCheckedMultiply(static_cast<std::uint64_t>(accessor.sparse.count),
				                          static_cast<std::uint64_t>(sparseIndexBytes),
				                          sparseWork)
				|| !editorCheckedAdd(decodeWorkBytes, sparseWork, decodeWorkBytes))
			{
				return fail(GltfValidationErrorCode::limitExceeded, base, "sparse accessor decode work overflows");
			}
		}
		if(decodeWorkBytes > m_limits.maxAuthorityPayloadBytes)
		{
			return fail(GltfValidationErrorCode::limitExceeded,
			            "accessors",
			            "aggregate decoded accessor work exceeds the configured payload limit");
		}
		return true;
	}

	void collectAccessorRequirements()
	{
		m_accessorRequirements.assign(m_model.accessors.size(), AccessorRequirements{});
		std::vector<std::uint64_t> meshJointUpperBounds(
			m_model.meshes.size(), std::numeric_limits<std::uint64_t>::max());
		for(const tinygltf::Node& node : m_model.nodes)
		{
			if(validIndex(node.mesh, m_model.meshes.size(), false)
				&& validIndex(node.skin, m_model.skins.size(), false))
			{
				std::uint64_t& upperBound = meshJointUpperBounds[static_cast<std::size_t>(node.mesh)];
				upperBound = std::min(
					upperBound,
					static_cast<std::uint64_t>(m_model.skins[static_cast<std::size_t>(node.skin)].joints.size()));
			}
		}
		for(std::size_t meshIndex = 0; meshIndex < m_model.meshes.size(); ++meshIndex)
		{
			const tinygltf::Mesh& mesh = m_model.meshes[meshIndex];
			for(const tinygltf::Primitive& primitive : mesh.primitives)
			{
				const auto position = primitive.attributes.find("POSITION");
				if(validIndex(primitive.indices, m_model.accessors.size(), false)
					&& position != primitive.attributes.end()
					&& validIndex(position->second, m_model.accessors.size(), false))
				{
					AccessorRequirements& requirements =
						m_accessorRequirements[static_cast<std::size_t>(primitive.indices)];
					requirements.primitiveIndices = true;
					requirements.primitiveIndexUpperBound = std::min(
						requirements.primitiveIndexUpperBound,
						static_cast<std::uint64_t>(m_model.accessors[static_cast<std::size_t>(position->second)].count));
				}
				for(const auto& [semantic, accessorIndex] : primitive.attributes)
				{
					if(!validIndex(accessorIndex, m_model.accessors.size(), false)) continue;
					AccessorRequirements& requirements = m_accessorRequirements[static_cast<std::size_t>(accessorIndex)];
					if(semantic.starts_with("WEIGHTS_")) requirements.weights = true;
					if(semantic.starts_with("JOINTS_"))
					{
						requirements.joints = true;
						requirements.jointUpperBound = std::min(
							requirements.jointUpperBound,
							meshJointUpperBounds[meshIndex]);
					}
				}
			}
		}
		for(const tinygltf::Animation& animation : m_model.animations)
		{
			for(const tinygltf::AnimationSampler& sampler : animation.samplers)
			{
				if(validIndex(sampler.input, m_model.accessors.size(), false))
				{
					m_accessorRequirements[static_cast<std::size_t>(sampler.input)].animationInput = true;
				}
			}
		}
	}

	[[nodiscard]] bool validateAccessorPayload(std::size_t index)
	{
		const tinygltf::Accessor& accessor = m_model.accessors[index];
		const AccessorRequirements& requirements = m_accessorRequirements[index];
		const int components = tinygltf::GetNumComponentsInType(static_cast<std::uint32_t>(accessor.type));
		const bool isUnsigned = unsignedComponentType(accessor.componentType);
		const bool validPrimitiveIndexFormat = requirements.primitiveIndices && isUnsigned
			&& accessor.type == TINYGLTF_TYPE_SCALAR && !accessor.normalized;
		const bool validJointFormat = requirements.joints && accessor.type == TINYGLTF_TYPE_VEC4
			&& !accessor.normalized
			&& (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE
				|| accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
		const bool validWeightFormat = requirements.weights && accessor.type == TINYGLTF_TYPE_VEC4
			&& ((accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && !accessor.normalized)
				|| (accessor.normalized
					&& (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE
						|| accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)));
		const bool validAnimationInputFormat = requirements.animationInput
			&& accessor.type == TINYGLTF_TYPE_SCALAR
			&& accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT;
		const std::uint64_t componentMaximum = unsignedComponentMaximum(accessor.componentType);
		const std::string base = "accessors[" + std::to_string(index) + "]";
		std::array<double, kMaxAccessorComponents> actualMinimum{};
		std::array<double, kMaxAccessorComponents> actualMaximum{};
		actualMinimum.fill(std::numeric_limits<double>::infinity());
		actualMaximum.fill(-std::numeric_limits<double>::infinity());
		double previousScalar = 0.0;
		bool havePreviousScalar = false;
		bool visitorFailed = false;
		GltfValidationErrorCode visitorCode = GltfValidationErrorCode::invalidAccessorPayload;
		std::string visitorPath = base;
		std::string visitorMessage;

		AccessorPayloadReader reader(m_model, accessor);
		const bool scanned = reader.scan(
			[&](std::uint64_t,
			    std::span<const double> values,
			    std::span<const std::uint64_t> rawValues)
			{
				std::uint64_t quantizedSum = 0;
				for(std::size_t component = 0; component < values.size(); ++component)
				{
					const double value = values[component];
					if(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && !std::isfinite(value))
					{
						visitorFailed = true;
						visitorMessage = "floating-point accessor payload contains NaN or infinity";
						return false;
					}
					actualMinimum[component] = std::min(actualMinimum[component], value);
					actualMaximum[component] = std::max(actualMaximum[component], value);
					if(validPrimitiveIndexFormat && rawValues[component] == componentMaximum)
					{
						visitorFailed = true;
						visitorCode = GltfValidationErrorCode::invalidPrimitiveIndex;
						visitorMessage = "primitive index payload contains the component-type restart sentinel";
						return false;
					}
					if(validPrimitiveIndexFormat && rawValues[component] >= requirements.primitiveIndexUpperBound)
					{
						visitorFailed = true;
						visitorCode = GltfValidationErrorCode::invalidPrimitiveIndex;
						visitorMessage = "primitive index payload exceeds its vertex domain";
						return false;
					}
					if(validJointFormat && requirements.jointUpperBound != std::numeric_limits<std::uint64_t>::max()
						&& rawValues[component] >= requirements.jointUpperBound)
					{
						visitorFailed = true;
						visitorCode = GltfValidationErrorCode::invalidJointIndex;
						visitorMessage = "joint index payload exceeds the skin joint domain";
						return false;
					}
					if(validWeightFormat && value < 0.0)
					{
						visitorFailed = true;
						visitorCode = GltfValidationErrorCode::invalidWeight;
						visitorMessage = "joint weight payload contains a negative value";
						return false;
					}
					if(validWeightFormat && isUnsigned
						&& !editorCheckedAdd(quantizedSum, rawValues[component], quantizedSum))
					{
						visitorFailed = true;
						visitorCode = GltfValidationErrorCode::invalidWeight;
						visitorMessage = "quantized joint weight sum overflows";
						return false;
					}
				}
				if(validWeightFormat && isUnsigned && quantizedSum != componentMaximum)
				{
					visitorFailed = true;
					visitorCode = GltfValidationErrorCode::invalidWeight;
					visitorMessage = "quantized joint weights do not sum to the component-type maximum";
					return false;
				}
				if(validAnimationInputFormat)
				{
					if(values[0] < 0.0)
					{
						visitorFailed = true;
						visitorCode = GltfValidationErrorCode::invalidAnimationInput;
						visitorMessage = "animation input timestamps must be non-negative";
						return false;
					}
					if(havePreviousScalar && !(values[0] > previousScalar))
					{
						visitorFailed = true;
						visitorCode = GltfValidationErrorCode::invalidAnimationInput;
						visitorMessage = "animation input timestamps must be strictly increasing";
						return false;
					}
					previousScalar = values[0];
					havePreviousScalar = true;
				}
				return true;
			});
		if(!scanned)
		{
			if(visitorFailed) return fail(visitorCode, std::move(visitorPath), std::move(visitorMessage));
			if(reader.error() == AccessorReadError::sparseIndexOutOfRange)
			{
				return fail(GltfValidationErrorCode::invalidSparseIndex,
				            base + ".sparse.indices",
				            "sparse index is outside the accessor element domain");
			}
			if(reader.error() == AccessorReadError::sparseIndicesNotIncreasing)
			{
				return fail(GltfValidationErrorCode::invalidSparseIndex,
				            base + ".sparse.indices",
				            "sparse indices must be strictly increasing");
			}
			return fail(GltfValidationErrorCode::invalidAccessorPayload,
			            base,
			            "accessor payload read exceeded its validated byte range");
		}

		for(std::size_t component = 0; component < static_cast<std::size_t>(components); ++component)
		{
			if(!accessor.minValues.empty()
				&& !accessorBoundMatches(accessor.componentType, accessor.minValues[component], actualMinimum[component]))
			{
				return fail(GltfValidationErrorCode::accessorBoundsMismatch,
				            base + ".min",
				            "declared accessor minimum does not match decoded payload");
			}
			if(!accessor.maxValues.empty()
				&& !accessorBoundMatches(accessor.componentType, accessor.maxValues[component], actualMaximum[component]))
			{
				return fail(GltfValidationErrorCode::accessorBoundsMismatch,
				            base + ".max",
				            "declared accessor maximum does not match decoded payload");
			}
		}
		return true;
	}

	[[nodiscard]] bool validateImagesAndTextures()
	{
		std::uint64_t authorityBytes = 0;
		for(const tinygltf::Buffer& buffer : m_model.buffers)
		{
			if(!editorCheckedAdd(authorityBytes, buffer.data.size(), authorityBytes))
			{
				return fail(GltfValidationErrorCode::limitExceeded, "buffers", "aggregate buffer payload overflows");
			}
		}
		for(std::size_t index = 0; index < m_model.images.size(); ++index)
		{
			const tinygltf::Image& image = m_model.images[index];
			const std::string base = "images[" + std::to_string(index) + "]";
			if(!checkReference(image.bufferView, m_model.bufferViews.size(), base + ".bufferView"))
			{
				return false;
			}
			if(!image.uri.empty() && image.bufferView >= 0)
			{
				return fail(GltfValidationErrorCode::invalidReference, base, "image cannot use both uri and bufferView");
			}
			if(image.uri.empty() && image.bufferView < 0 && image.image.empty())
			{
				return fail(GltfValidationErrorCode::invalidReference,
				            base,
				            "image requires a URI, bufferView, or decoded data-URI payload");
			}
			if(image.bufferView >= 0 && image.mimeType.empty())
			{
				return fail(GltfValidationErrorCode::invalidValue, base + ".mimeType", "bufferView image requires a MIME type");
			}
			const std::uint64_t bytes = static_cast<std::uint64_t>(image.image.size());
			if(bytes == 0 || bytes > m_limits.maxImageBytes
				|| !editorCheckedAdd(authorityBytes, bytes, authorityBytes)
				|| authorityBytes > m_limits.maxAuthorityPayloadBytes)
			{
				return fail(GltfValidationErrorCode::limitExceeded, base + ".image", "encoded image payload exceeds configured limits");
			}
			if((image.width > 0 && static_cast<std::size_t>(image.width) > m_limits.maxImageDimension)
				|| (image.height > 0 && static_cast<std::size_t>(image.height) > m_limits.maxImageDimension))
			{
				return fail(GltfValidationErrorCode::limitExceeded, base, "image dimensions exceed configured limits");
			}
		}

		const auto validMagFilter = [](int value)
		{
			return value == -1 || value == TINYGLTF_TEXTURE_FILTER_NEAREST || value == TINYGLTF_TEXTURE_FILTER_LINEAR;
		};
		const auto validMinFilter = [](int value)
		{
			return value == -1 || value == TINYGLTF_TEXTURE_FILTER_NEAREST || value == TINYGLTF_TEXTURE_FILTER_LINEAR
				|| value == TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST
				|| value == TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST
				|| value == TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR
				|| value == TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR;
		};
		const auto validWrap = [](int value)
		{
			return value == TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE
				|| value == TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT
				|| value == TINYGLTF_TEXTURE_WRAP_REPEAT;
		};
		for(std::size_t index = 0; index < m_model.samplers.size(); ++index)
		{
			const tinygltf::Sampler& sampler = m_model.samplers[index];
			if(!validMagFilter(sampler.magFilter) || !validMinFilter(sampler.minFilter)
				|| !validWrap(sampler.wrapS) || !validWrap(sampler.wrapT))
			{
				return fail(GltfValidationErrorCode::invalidValue,
				            "samplers[" + std::to_string(index) + "]",
				            "sampler filter or wrap mode is invalid");
			}
		}

		for(std::size_t index = 0; index < m_model.textures.size(); ++index)
		{
			const tinygltf::Texture& texture = m_model.textures[index];
			const std::string base = "textures[" + std::to_string(index) + "]";
			const bool extensionSource = texture.extensions.contains("KHR_texture_basisu")
				|| texture.extensions.contains("EXT_texture_webp")
				|| texture.extensions.contains("EXT_texture_avif")
				|| texture.extensions.contains("MSFT_texture_dds");
			if(!checkReference(texture.sampler, m_model.samplers.size(), base + ".sampler")
				|| (!extensionSource && !checkReference(texture.source, m_model.images.size(), base + ".source", false))
				|| (extensionSource && !checkReference(texture.source, m_model.images.size(), base + ".source")))
			{
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] bool validateMaterialsAndMeshes()
	{
		const auto checkTexture = [this](int index, const std::string& path)
		{
			return checkReference(index, m_model.textures.size(), path);
		};
		for(std::size_t index = 0; index < m_model.materials.size(); ++index)
		{
			const tinygltf::Material& material = m_model.materials[index];
			const std::string base = "materials[" + std::to_string(index) + "]";
			if(!checkTexture(material.pbrMetallicRoughness.baseColorTexture.index, base + ".pbr.baseColorTexture")
				|| !checkTexture(material.pbrMetallicRoughness.metallicRoughnessTexture.index, base + ".pbr.metallicRoughnessTexture")
				|| !checkTexture(material.normalTexture.index, base + ".normalTexture")
				|| !checkTexture(material.occlusionTexture.index, base + ".occlusionTexture")
				|| !checkTexture(material.emissiveTexture.index, base + ".emissiveTexture"))
			{
				return false;
			}
			const auto& pbr = material.pbrMetallicRoughness;
			const auto inUnitRange = [](double value) { return std::isfinite(value) && value >= 0.0 && value <= 1.0; };
			if(pbr.baseColorFactor.size() != 4
				|| !std::all_of(pbr.baseColorFactor.begin(), pbr.baseColorFactor.end(), inUnitRange)
				|| !inUnitRange(pbr.metallicFactor)
				|| !inUnitRange(pbr.roughnessFactor)
				|| material.emissiveFactor.size() != 3
				|| !std::all_of(material.emissiveFactor.begin(), material.emissiveFactor.end(), [](double value) {
					return std::isfinite(value) && value >= 0.0;
				})
				|| !std::isfinite(material.alphaCutoff) || material.alphaCutoff < 0.0
				|| !std::isfinite(material.normalTexture.scale)
				|| !inUnitRange(material.occlusionTexture.strength))
			{
				return fail(GltfValidationErrorCode::invalidValue, base, "material factors are outside glTF ranges");
			}
			if(material.alphaMode != "OPAQUE" && material.alphaMode != "MASK" && material.alphaMode != "BLEND")
			{
				return fail(GltfValidationErrorCode::invalidValue, base + ".alphaMode", "material alpha mode is invalid");
			}
			for(const auto* textureInfo : {
				&pbr.baseColorTexture,
				&pbr.metallicRoughnessTexture,
				&material.emissiveTexture,
			})
			{
				if(textureInfo->texCoord < 0)
				{
					return fail(GltfValidationErrorCode::invalidValue, base, "texture coordinate set index is negative");
				}
			}
			if(material.normalTexture.texCoord < 0 || material.occlusionTexture.texCoord < 0)
			{
				return fail(GltfValidationErrorCode::invalidValue, base, "texture coordinate set index is negative");
			}
			for(std::size_t lodIndex = 0; lodIndex < material.lods.size(); ++lodIndex)
			{
				if(!checkReference(material.lods[lodIndex],
				                   m_model.materials.size(),
				                   base + ".lods[" + std::to_string(lodIndex) + "]",
				                   false)) return false;
			}
		}

		for(std::size_t meshIndex = 0; meshIndex < m_model.meshes.size(); ++meshIndex)
		{
			const tinygltf::Mesh& mesh = m_model.meshes[meshIndex];
			if(mesh.primitives.empty())
			{
				return fail(GltfValidationErrorCode::invalidReference,
				            "meshes[" + std::to_string(meshIndex) + "].primitives",
				            "mesh requires at least one primitive");
			}
			if(!finiteValues(mesh.weights))
			{
				return fail(GltfValidationErrorCode::invalidValue,
				            "meshes[" + std::to_string(meshIndex) + "].weights",
				            "mesh morph weights contain a non-finite value");
			}
			const std::size_t meshTargetCount = mesh.primitives.front().targets.size();
			if(!mesh.weights.empty() && mesh.weights.size() != meshTargetCount)
			{
				return fail(GltfValidationErrorCode::invalidValue,
				            "meshes[" + std::to_string(meshIndex) + "].weights",
				            "mesh weights must match the primitive morph-target count");
			}
			for(std::size_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex)
			{
				const tinygltf::Primitive& primitive = mesh.primitives[primitiveIndex];
				const std::string base = "meshes[" + std::to_string(meshIndex) + "].primitives["
					+ std::to_string(primitiveIndex) + "]";
				if(primitive.mode < TINYGLTF_MODE_POINTS || primitive.mode > TINYGLTF_MODE_TRIANGLE_FAN)
				{
					return fail(GltfValidationErrorCode::invalidValue, base + ".mode", "primitive mode is invalid");
				}
				if(!checkReference(primitive.indices, m_model.accessors.size(), base + ".indices")
					|| !checkReference(primitive.material, m_model.materials.size(), base + ".material"))
				{
					return false;
				}
				if(primitive.attributes.empty())
				{
					return fail(GltfValidationErrorCode::invalidReference, base + ".attributes", "primitive attributes are required");
				}
				if(!primitive.attributes.contains("POSITION"))
				{
					return fail(GltfValidationErrorCode::invalidReference,
					            base + ".attributes.POSITION",
					            "mesh primitive requires a POSITION accessor");
				}
				const std::size_t positionIndex = static_cast<std::size_t>(primitive.attributes.at("POSITION"));
				if(positionIndex >= m_model.accessors.size())
				{
					return fail(GltfValidationErrorCode::indexOutOfRange,
					            base + ".attributes.POSITION",
					            "POSITION accessor index is outside the accessor collection");
				}
				const tinygltf::Accessor& position = m_model.accessors[positionIndex];
				const std::uint64_t vertexCount = position.count;
				if(position.type != TINYGLTF_TYPE_VEC3
					|| position.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT
					|| position.minValues.empty() || position.maxValues.empty())
				{
					return fail(GltfValidationErrorCode::invalidAccessor,
					            base + ".attributes.POSITION",
					            "POSITION must be a FLOAT VEC3 accessor with declared bounds");
				}
				std::uint64_t topologyCount = vertexCount;
				if(primitive.indices >= 0)
				{
					const auto& indices = m_model.accessors[static_cast<std::size_t>(primitive.indices)];
					if(indices.type != TINYGLTF_TYPE_SCALAR || indices.normalized
						|| (indices.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE
							&& indices.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT
							&& indices.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT))
					{
						return fail(GltfValidationErrorCode::invalidReference,
						            base + ".indices",
						            "primitive indices must reference a non-normalized unsigned integer SCALAR accessor");
					}
					topologyCount = indices.count;
				}
				if(!validPrimitiveCount(primitive.mode, topologyCount))
				{
					return fail(GltfValidationErrorCode::invalidPrimitiveTopology,
					            base,
					            "primitive element count is incompatible with its topology mode");
				}
				if(primitive.targets.size() != meshTargetCount)
				{
					return fail(GltfValidationErrorCode::invalidValue,
					            base + ".targets",
					            "all mesh primitives must have the same morph-target count");
				}
				std::unordered_set<std::string> jointSets;
				std::unordered_set<std::string> weightSets;
				for(const auto& [semantic, accessor] : primitive.attributes)
				{
					if(semantic.empty() || !checkReference(accessor, m_model.accessors.size(), base + ".attributes." + semantic, false))
					{
						return m_report.valid()
							? fail(GltfValidationErrorCode::invalidReference, base + ".attributes", "attribute semantic is empty")
							: false;
					}
					const tinygltf::Accessor& attribute = m_model.accessors[static_cast<std::size_t>(accessor)];
					if(attribute.count != vertexCount)
					{
						return fail(GltfValidationErrorCode::attributeCountMismatch,
						            base + ".attributes." + semantic,
						            "all primitive attribute accessors must have the POSITION count");
					}
					const bool floatVec3 = attribute.type == TINYGLTF_TYPE_VEC3
						&& attribute.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && !attribute.normalized;
					const bool floatVec4 = attribute.type == TINYGLTF_TYPE_VEC4
						&& attribute.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && !attribute.normalized;
					const bool normalizedU8OrU16 = attribute.normalized
						&& (attribute.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE
							|| attribute.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
					if((semantic == "POSITION" || semantic == "NORMAL") && !floatVec3)
					{
						return fail(GltfValidationErrorCode::invalidAccessor,
						            base + ".attributes." + semantic,
						            semantic + " must be a FLOAT VEC3 accessor");
					}
					if(semantic == "TANGENT" && !floatVec4)
					{
						return fail(GltfValidationErrorCode::invalidAccessor,
						            base + ".attributes.TANGENT",
						            "TANGENT must be a FLOAT VEC4 accessor");
					}
					if(semantic.starts_with("TEXCOORD_")
						&& !(attribute.type == TINYGLTF_TYPE_VEC2
							&& ((attribute.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && !attribute.normalized)
								|| normalizedU8OrU16)))
					{
						return fail(GltfValidationErrorCode::invalidAccessor,
						            base + ".attributes." + semantic,
						            "texture coordinates use an invalid accessor format");
					}
					if(semantic.starts_with("COLOR_")
						&& !((attribute.type == TINYGLTF_TYPE_VEC3 || attribute.type == TINYGLTF_TYPE_VEC4)
							&& ((attribute.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && !attribute.normalized)
								|| normalizedU8OrU16)))
					{
						return fail(GltfValidationErrorCode::invalidAccessor,
						            base + ".attributes." + semantic,
						            "vertex colors use an invalid accessor format");
					}
					if(semantic.starts_with("JOINTS_"))
					{
						if(attribute.type != TINYGLTF_TYPE_VEC4 || attribute.normalized
							|| (attribute.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE
								&& attribute.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT))
						{
							return fail(GltfValidationErrorCode::invalidAccessor,
							            base + ".attributes." + semantic,
							            "joint indices use an invalid accessor format");
						}
						jointSets.insert(semantic.substr(std::string("JOINTS_").size()));
					}
					if(semantic.starts_with("WEIGHTS_"))
					{
						if(attribute.type != TINYGLTF_TYPE_VEC4
							|| !((attribute.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && !attribute.normalized)
								|| normalizedU8OrU16))
						{
							return fail(GltfValidationErrorCode::invalidAccessor,
							            base + ".attributes." + semantic,
							            "joint weights use an invalid accessor format");
						}
						weightSets.insert(semantic.substr(std::string("WEIGHTS_").size()));
					}
				}
				if(jointSets != weightSets)
				{
					return fail(GltfValidationErrorCode::invalidReference,
					            base + ".attributes",
					            "JOINTS and WEIGHTS attribute sets must match");
				}
				for(std::size_t targetIndex = 0; targetIndex < primitive.targets.size(); ++targetIndex)
				{
					for(const auto& [semantic, accessor] : primitive.targets[targetIndex])
					{
						if(semantic.empty()
							|| !checkReference(accessor,
							                   m_model.accessors.size(),
							                   base + ".targets[" + std::to_string(targetIndex) + "]." + semantic,
							                   false))
						{
							return m_report.valid()
								? fail(GltfValidationErrorCode::invalidReference, base + ".targets", "target semantic is empty")
								: false;
						}
						const tinygltf::Accessor& target = m_model.accessors[static_cast<std::size_t>(accessor)];
						if(target.count != vertexCount)
						{
							return fail(GltfValidationErrorCode::attributeCountMismatch,
							            base + ".targets[" + std::to_string(targetIndex) + "]." + semantic,
							            "morph target accessors must have the primitive vertex count");
						}
						if((semantic != "POSITION" && semantic != "NORMAL" && semantic != "TANGENT")
							|| target.type != TINYGLTF_TYPE_VEC3
							|| target.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT || target.normalized)
						{
							return fail(GltfValidationErrorCode::invalidAccessor,
							            base + ".targets[" + std::to_string(targetIndex) + "]." + semantic,
							            "morph target attributes must be FLOAT VEC3 position, normal, or tangent deltas");
						}
					}
				}
			}
		}
		return true;
	}

	[[nodiscard]] bool validateNodesAndScenes()
	{
		if(!checkReference(m_model.defaultScene, m_model.scenes.size(), "scene"))
		{
			return false;
		}

		std::vector<std::uint32_t> parentCount(m_model.nodes.size(), 0);
		std::vector<std::vector<std::size_t>> adjacency(m_model.nodes.size());
		std::uint64_t edgeCount = 0;
		for(std::size_t nodeIndex = 0; nodeIndex < m_model.nodes.size(); ++nodeIndex)
		{
			const tinygltf::Node& node = m_model.nodes[nodeIndex];
			const std::string base = "nodes[" + std::to_string(nodeIndex) + "]";
			if(!checkReference(node.mesh, m_model.meshes.size(), base + ".mesh")
				|| !checkReference(node.skin, m_model.skins.size(), base + ".skin")
				|| !checkReference(node.camera, m_model.cameras.size(), base + ".camera")
				|| !checkReference(node.light, m_model.lights.size(), base + ".light")
				|| !checkReference(node.emitter, m_model.audioEmitters.size(), base + ".emitter"))
			{
				return false;
			}
			if(!node.matrix.empty()
				&& (!node.translation.empty() || !node.rotation.empty() || !node.scale.empty()))
			{
				return fail(GltfValidationErrorCode::invalidTransform, base, "matrix and TRS are mutually exclusive");
			}
			if((!node.matrix.empty() && node.matrix.size() != 16)
				|| (!node.translation.empty() && node.translation.size() != 3)
				|| (!node.rotation.empty() && node.rotation.size() != 4)
				|| (!node.scale.empty() && node.scale.size() != 3)
				|| !finiteValues(node.matrix)
				|| !finiteValues(node.translation)
				|| !finiteValues(node.rotation)
				|| !finiteValues(node.scale)
				|| !finiteValues(node.weights))
			{
				return fail(GltfValidationErrorCode::invalidTransform, base, "transform vector has invalid size or non-finite value");
			}
			if(!node.rotation.empty())
			{
				double squaredLength = 0.0;
				for(const double component : node.rotation) squaredLength += component * component;
				if(!std::isfinite(squaredLength) || std::abs(squaredLength - 1.0) > 1.0e-5)
				{
					return fail(GltfValidationErrorCode::invalidTransform, base + ".rotation", "rotation quaternion is not normalized");
				}
			}
			if(node.skin >= 0 && node.mesh < 0)
			{
				return fail(GltfValidationErrorCode::invalidReference,
				            base + ".skin",
				            "node skin requires a mesh");
			}
			if(node.mesh < 0 && !node.weights.empty())
			{
				return fail(GltfValidationErrorCode::invalidReference,
				            base + ".weights",
				            "node morph weights require a mesh");
			}
			if(node.mesh >= 0 && !node.weights.empty())
			{
				const tinygltf::Mesh& mesh = m_model.meshes[static_cast<std::size_t>(node.mesh)];
				const std::size_t targetCount = mesh.primitives.empty() ? 0 : mesh.primitives.front().targets.size();
				if(node.weights.size() != targetCount)
				{
					return fail(GltfValidationErrorCode::invalidValue, base + ".weights", "node morph weights do not match its mesh");
				}
			}

			adjacency[nodeIndex].reserve(node.children.size());
			for(std::size_t childOffset = 0; childOffset < node.children.size(); ++childOffset)
			{
				const int child = node.children[childOffset];
				if(!checkReference(child,
				                   m_model.nodes.size(),
				                   base + ".children[" + std::to_string(childOffset) + "]",
				                   false))
				{
					return false;
				}
				if(!editorCheckedAdd(edgeCount, 1, edgeCount) || edgeCount > m_limits.maxNodeEdges)
				{
					return fail(GltfValidationErrorCode::limitExceeded, "nodes.children", "node edge count exceeds limit");
				}
				const std::size_t childIndex = static_cast<std::size_t>(child);
				if(++parentCount[childIndex] > 1)
				{
					return fail(GltfValidationErrorCode::duplicateParent,
					            base + ".children[" + std::to_string(childOffset) + "]",
					            "a node may have at most one node parent");
				}
				adjacency[nodeIndex].push_back(childIndex);
			}

			for(std::size_t lodIndex = 0; lodIndex < node.lods.size(); ++lodIndex)
			{
				if(!checkReference(node.lods[lodIndex],
				                   m_model.nodes.size(),
				                   base + ".lods[" + std::to_string(lodIndex) + "]",
				                   false))
				{
					return false;
				}
			}
		}

		std::vector<std::size_t> remainingParents(parentCount.begin(), parentCount.end());
		std::vector<std::size_t> depth(m_model.nodes.size(), 1);
		std::vector<std::size_t> queue;
		queue.reserve(m_model.nodes.size());
		for(std::size_t index = 0; index < remainingParents.size(); ++index)
		{
			if(remainingParents[index] == 0)
			{
				queue.push_back(index);
			}
		}
		std::size_t head = 0;
		std::size_t visited = 0;
		while(head < queue.size())
		{
			const std::size_t parent = queue[head++];
			++visited;
			if(depth[parent] > m_limits.maxNodeDepth)
			{
				return fail(GltfValidationErrorCode::depthExceeded,
				            "nodes[" + std::to_string(parent) + "]",
				            "node graph depth exceeds limit");
			}
			for(const std::size_t child : adjacency[parent])
			{
				depth[child] = std::max(depth[child], depth[parent] + 1);
				if(--remainingParents[child] == 0)
				{
					queue.push_back(child);
				}
			}
		}
		if(visited != m_model.nodes.size())
		{
			return fail(GltfValidationErrorCode::cycle, "nodes", "node graph contains a cycle");
		}

		std::vector<std::uint32_t> sceneMarks(m_model.nodes.size(), 0);
		std::uint32_t sceneMark = 0;
		std::uint64_t sceneOccurrences = 0;
		for(std::size_t sceneIndex = 0; sceneIndex < m_model.scenes.size(); ++sceneIndex)
		{
			const tinygltf::Scene& scene = m_model.scenes[sceneIndex];
			std::unordered_set<int> roots;
			roots.reserve(scene.nodes.size());
			for(std::size_t rootIndex = 0; rootIndex < scene.nodes.size(); ++rootIndex)
			{
				const int node = scene.nodes[rootIndex];
				const std::string path = "scenes[" + std::to_string(sceneIndex) + "].nodes["
					+ std::to_string(rootIndex) + "]";
				if(!checkReference(node, m_model.nodes.size(), path, false))
				{
					return false;
				}
				if(!roots.insert(node).second)
				{
					return fail(GltfValidationErrorCode::duplicateParent, path, "scene contains a duplicate root occurrence");
				}
			}
			++sceneMark;
			std::vector<std::size_t> pending;
			pending.reserve(scene.nodes.size());
			for(const int root : scene.nodes) pending.push_back(static_cast<std::size_t>(root));
			while(!pending.empty())
			{
				const std::size_t node = pending.back();
				pending.pop_back();
				if(sceneMarks[node] == sceneMark)
				{
					return fail(GltfValidationErrorCode::duplicateParent,
					            "scenes[" + std::to_string(sceneIndex) + "]",
					            "scene reaches the same node through more than one root occurrence");
				}
				sceneMarks[node] = sceneMark;
				if(!editorCheckedAdd(sceneOccurrences, 1, sceneOccurrences)
					|| sceneOccurrences > m_limits.maxSceneNodeOccurrences)
				{
					return fail(GltfValidationErrorCode::limitExceeded, "scenes", "scene node occurrence work exceeds its limit");
				}
				for(const std::size_t child : adjacency[node]) pending.push_back(child);
			}
			for(std::size_t emitterIndex = 0; emitterIndex < scene.audioEmitters.size(); ++emitterIndex)
			{
				if(!checkReference(scene.audioEmitters[emitterIndex],
				                   m_model.audioEmitters.size(),
				                   "scenes[" + std::to_string(sceneIndex) + "].audioEmitters["
				                   + std::to_string(emitterIndex) + "]",
				                   false)) return false;
			}
		}

		return true;
	}

	[[nodiscard]] bool validateSkinsAndAnimations()
	{
		for(std::size_t index = 0; index < m_model.skins.size(); ++index)
		{
			const tinygltf::Skin& skin = m_model.skins[index];
			const std::string base = "skins[" + std::to_string(index) + "]";
			if(!checkReference(skin.inverseBindMatrices, m_model.accessors.size(), base + ".inverseBindMatrices")
				|| !checkReference(skin.skeleton, m_model.nodes.size(), base + ".skeleton"))
			{
				return false;
			}
			if(skin.joints.empty())
			{
				return fail(GltfValidationErrorCode::invalidReference, base + ".joints", "skin requires at least one joint");
			}
			if(skin.inverseBindMatrices >= 0)
			{
				const tinygltf::Accessor& matrices = m_model.accessors[static_cast<std::size_t>(skin.inverseBindMatrices)];
				if(matrices.type != TINYGLTF_TYPE_MAT4
					|| matrices.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT
					|| matrices.count != skin.joints.size())
				{
					return fail(GltfValidationErrorCode::invalidReference,
					            base + ".inverseBindMatrices",
					            "inverse bind accessor must be FLOAT MAT4 with one entry per joint");
				}
			}
			std::unordered_set<int> joints;
			joints.reserve(skin.joints.size());
			for(std::size_t jointIndex = 0; jointIndex < skin.joints.size(); ++jointIndex)
			{
				const int joint = skin.joints[jointIndex];
				if(!checkReference(joint,
				                   m_model.nodes.size(),
				                   base + ".joints[" + std::to_string(jointIndex) + "]",
				                   false))
				{
					return false;
				}
				if(!joints.insert(joint).second)
				{
					return fail(GltfValidationErrorCode::invalidReference, base + ".joints", "skin joints must be unique");
				}
			}
		}

		for(std::size_t animationIndex = 0; animationIndex < m_model.animations.size(); ++animationIndex)
		{
			const tinygltf::Animation& animation = m_model.animations[animationIndex];
			const std::string base = "animations[" + std::to_string(animationIndex) + "]";
			if(animation.samplers.empty() || animation.channels.empty())
			{
				return fail(GltfValidationErrorCode::invalidReference,
				            base,
				            "animation requires nonempty sampler and channel collections");
			}
			for(std::size_t samplerIndex = 0; samplerIndex < animation.samplers.size(); ++samplerIndex)
			{
				const tinygltf::AnimationSampler& sampler = animation.samplers[samplerIndex];
				if(!checkReference(sampler.input,
				                   m_model.accessors.size(),
				                   base + ".samplers[" + std::to_string(samplerIndex) + "].input",
				                   false)
					|| !checkReference(sampler.output,
					                   m_model.accessors.size(),
					                   base + ".samplers[" + std::to_string(samplerIndex) + "].output",
					                   false))
				{
					return false;
				}
				if(sampler.interpolation != "LINEAR" && sampler.interpolation != "STEP"
					&& sampler.interpolation != "CUBICSPLINE")
				{
					return fail(GltfValidationErrorCode::invalidValue,
					            base + ".samplers[" + std::to_string(samplerIndex) + "].interpolation",
					            "animation interpolation is invalid");
				}
				const tinygltf::Accessor& input = m_model.accessors[static_cast<std::size_t>(sampler.input)];
				if(input.type != TINYGLTF_TYPE_SCALAR || input.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
				{
					return fail(GltfValidationErrorCode::invalidReference,
					            base + ".samplers[" + std::to_string(samplerIndex) + "].input",
					            "animation input accessor must be FLOAT SCALAR");
				}
			}
			std::unordered_set<std::string> targetChannels;
			targetChannels.reserve(animation.channels.size());
			for(std::size_t channelIndex = 0; channelIndex < animation.channels.size(); ++channelIndex)
			{
				const tinygltf::AnimationChannel& channel = animation.channels[channelIndex];
				const std::string channelBase = base + ".channels[" + std::to_string(channelIndex) + "]";
				if(!checkReference(channel.sampler, animation.samplers.size(), channelBase + ".sampler", false)
					|| !checkReference(channel.target_node, m_model.nodes.size(), channelBase + ".target.node"))
				{
					return false;
				}
				if(channel.target_extensions.empty()
					&& channel.target_path != "translation" && channel.target_path != "rotation"
					&& channel.target_path != "scale" && channel.target_path != "weights")
				{
					return fail(GltfValidationErrorCode::invalidValue,
					            channelBase + ".target.path",
					            "animation target path is invalid");
				}
				if(channel.target_node < 0 && channel.target_extensions.empty())
				{
					return fail(GltfValidationErrorCode::invalidReference,
					            channelBase + ".target",
					            "animation channel requires a node or extension target");
				}
				if(channel.target_node >= 0)
				{
					const std::string targetKey = std::to_string(channel.target_node) + ':' + channel.target_path;
					if(!targetChannels.insert(targetKey).second)
					{
						return fail(GltfValidationErrorCode::invalidReference,
						            channelBase + ".target",
						            "animation contains duplicate channels for the same node property");
					}
				}
				if(channel.target_node >= 0)
				{
					const tinygltf::AnimationSampler& sampler = animation.samplers[static_cast<std::size_t>(channel.sampler)];
					const tinygltf::Accessor& input = m_model.accessors[static_cast<std::size_t>(sampler.input)];
					const tinygltf::Accessor& output = m_model.accessors[static_cast<std::size_t>(sampler.output)];
					const int expectedType = channel.target_path == "rotation"
						? TINYGLTF_TYPE_VEC4
						: (channel.target_path == "weights" ? TINYGLTF_TYPE_SCALAR : TINYGLTF_TYPE_VEC3);
					std::uint64_t expectedCount = input.count;
					if(channel.target_path == "weights")
					{
						const tinygltf::Node& node = m_model.nodes[static_cast<std::size_t>(channel.target_node)];
						if(node.mesh < 0)
						{
							return fail(GltfValidationErrorCode::invalidReference, channelBase, "weights animation targets a node without a mesh");
						}
						const tinygltf::Mesh& mesh = m_model.meshes[static_cast<std::size_t>(node.mesh)];
						const std::size_t targets = mesh.primitives.empty() ? 0 : mesh.primitives.front().targets.size();
						if(targets == 0 || !editorCheckedMultiply(expectedCount, targets, expectedCount))
						{
							return fail(GltfValidationErrorCode::invalidReference, channelBase, "weights animation target count is invalid");
						}
					}
					if(sampler.interpolation == "CUBICSPLINE"
						&& !editorCheckedMultiply(expectedCount, 3, expectedCount))
					{
						return fail(GltfValidationErrorCode::limitExceeded, channelBase, "animation output count overflows");
					}
					if(output.type != expectedType || output.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT
						|| output.count != expectedCount)
					{
						return fail(GltfValidationErrorCode::invalidReference,
						            channelBase,
						            "animation output accessor type or count does not match its target");
					}
				}
			}
		}
		return true;
	}

	[[nodiscard]] bool validateCamerasLightsAndAudio()
	{
		for(std::size_t index = 0; index < m_model.cameras.size(); ++index)
		{
			const tinygltf::Camera& camera = m_model.cameras[index];
			const std::string base = "cameras[" + std::to_string(index) + "]";
			if(camera.type == "perspective")
			{
				const auto& value = camera.perspective;
				if(!std::isfinite(value.aspectRatio) || !std::isfinite(value.yfov)
					|| !std::isfinite(value.znear) || !std::isfinite(value.zfar)
					|| value.aspectRatio < 0.0 || value.yfov <= 0.0 || value.yfov >= 3.14159265358979323846
					|| value.znear <= 0.0
					|| (value.zfar != 0.0 && value.zfar <= value.znear))
				{
					return fail(GltfValidationErrorCode::invalidValue, base, "perspective camera parameters are invalid");
				}
			}
			else if(camera.type == "orthographic")
			{
				const auto& value = camera.orthographic;
				if(!std::isfinite(value.xmag) || !std::isfinite(value.ymag)
					|| !std::isfinite(value.znear) || !std::isfinite(value.zfar)
					|| value.xmag <= 0.0 || value.ymag <= 0.0 || value.znear < 0.0 || value.zfar <= value.znear)
				{
					return fail(GltfValidationErrorCode::invalidValue, base, "orthographic camera parameters are invalid");
				}
			}
			else
			{
				return fail(GltfValidationErrorCode::invalidValue, base + ".type", "camera type is invalid");
			}
		}

		for(std::size_t index = 0; index < m_model.lights.size(); ++index)
		{
			const tinygltf::Light& light = m_model.lights[index];
			const std::string base = "lights[" + std::to_string(index) + "]";
			if(light.type != "point" && light.type != "spot" && light.type != "directional")
			{
				return fail(GltfValidationErrorCode::invalidValue, base + ".type", "light type is invalid");
			}
			if(!std::isfinite(light.intensity) || light.intensity < 0.0
				|| !std::isfinite(light.range) || light.range < 0.0
				|| (!light.color.empty() && light.color.size() != 3) || !finiteValues(light.color)
				|| !std::all_of(light.color.begin(), light.color.end(), [](double value) { return value >= 0.0; })
				|| !std::isfinite(light.spot.innerConeAngle)
				|| !std::isfinite(light.spot.outerConeAngle)
				|| light.spot.innerConeAngle < 0.0
				|| light.spot.outerConeAngle <= 0.0
				|| light.spot.outerConeAngle > 1.5707963267948966
				|| light.spot.innerConeAngle > light.spot.outerConeAngle)
			{
				return fail(GltfValidationErrorCode::invalidValue, base, "light contains non-finite values");
			}
		}

		for(std::size_t index = 0; index < m_model.audioSources.size(); ++index)
		{
			const tinygltf::AudioSource& source = m_model.audioSources[index];
			const std::string base = "audioSources[" + std::to_string(index) + "]";
			if(!checkReference(source.bufferView, m_model.bufferViews.size(), base + ".bufferView")) return false;
			if(source.uri.empty() == (source.bufferView < 0))
			{
				return fail(GltfValidationErrorCode::invalidReference, base, "audio source requires exactly one of uri or bufferView");
			}
			if(!source.uri.empty())
			{
				return fail(GltfValidationErrorCode::invalidValue,
				            base + ".uri",
				            "external and data-URI audio are unsupported until audio dependencies are staged");
			}
			if(source.bufferView >= 0 && source.mimeType.empty())
			{
				return fail(GltfValidationErrorCode::invalidValue, base + ".mimeType", "bufferView audio source requires a MIME type");
			}
		}
		for(std::size_t index = 0; index < m_model.audioEmitters.size(); ++index)
		{
			const tinygltf::AudioEmitter& emitter = m_model.audioEmitters[index];
			const std::string base = "audioEmitters[" + std::to_string(index) + "]";
			if(!checkReference(emitter.source, m_model.audioSources.size(), base + ".source", false)) return false;
			if((emitter.type != "global" && emitter.type != "positional")
				|| (emitter.distanceModel != "linear" && emitter.distanceModel != "inverse"
					&& emitter.distanceModel != "exponential")
				|| !std::isfinite(emitter.gain) || emitter.gain < 0.0
				|| !std::isfinite(emitter.positional.coneInnerAngle)
				|| !std::isfinite(emitter.positional.coneOuterAngle)
				|| !std::isfinite(emitter.positional.coneOuterGain)
				|| !std::isfinite(emitter.positional.maxDistance)
				|| !std::isfinite(emitter.positional.refDistance)
				|| !std::isfinite(emitter.positional.rolloffFactor)
				|| emitter.positional.coneInnerAngle < 0.0
				|| emitter.positional.coneOuterAngle < emitter.positional.coneInnerAngle
				|| emitter.positional.coneOuterGain < 0.0 || emitter.positional.coneOuterGain > 1.0
				|| emitter.positional.maxDistance <= 0.0 || emitter.positional.refDistance <= 0.0
				|| emitter.positional.rolloffFactor < 0.0)
			{
				return fail(GltfValidationErrorCode::invalidValue, base, "audio emitter parameters are invalid");
			}
		}
		return true;
	}

	[[nodiscard]] bool validateIntegerReference(
		const tinygltf::Value& value,
		std::size_t count,
		std::string path,
		bool optional = false)
	{
		if(!value.IsInt())
		{
			return fail(GltfValidationErrorCode::invalidReference,
			            std::move(path),
			            "extension reference must be an integer");
		}
		return checkReference(value.GetNumberAsInt(), count, std::move(path), optional);
	}

	[[nodiscard]] bool validateTextureInfoExtension(
		const tinygltf::Value& extension,
		std::string_view member,
		const std::string& path,
		bool normalTexture = false)
	{
		if(!extension.IsObject())
		{
			return fail(GltfValidationErrorCode::invalidValue,
			            path,
			            "known material extension must be an object");
		}
		if(!extension.Has(std::string(member)))
		{
			return true;
		}
		const tinygltf::Value& info = extension.Get(std::string(member));
		if(!info.IsObject() || !info.Has("index"))
		{
			return fail(GltfValidationErrorCode::invalidReference,
			            path + "." + std::string(member),
			            "extension texture info requires an integer index");
		}
		if(!validateIntegerReference(
			info.Get("index"),
			m_model.textures.size(),
			path + "." + std::string(member) + ".index"))
		{
			return false;
		}
		const std::string infoPath = path + "." + std::string(member);
		if(info.Has("texCoord"))
		{
			const auto& texCoord = info.Get("texCoord");
			if(!texCoord.IsInt() || texCoord.GetNumberAsInt() < 0)
			{
				return fail(GltfValidationErrorCode::invalidValue,
				            infoPath + ".texCoord",
				            "texture coordinate set must be a nonnegative integer");
			}
		}
		if(normalTexture && info.Has("scale") && !info.Get("scale").IsNumber())
		{
			return fail(GltfValidationErrorCode::invalidValue,
			            infoPath + ".scale",
			            "normal texture scale must be a number");
		}
		if(!info.Has("extensions")) return true;
		const auto& extensions = info.Get("extensions");
		if(!extensions.IsObject())
		{
			return fail(GltfValidationErrorCode::invalidValue,
			            infoPath + ".extensions",
			            "texture info extensions must be an object");
		}
		if(!extensions.Has("KHR_texture_transform")) return true;
		const auto& transform = extensions.Get("KHR_texture_transform");
		const std::string transformPath = infoPath + ".extensions.KHR_texture_transform";
		if(!transform.IsObject())
		{
			return fail(GltfValidationErrorCode::invalidValue,
			            transformPath,
			            "texture transform must be an object");
		}
		for(const std::string_view vectorMember : {
			std::string_view{"offset"}, std::string_view{"scale"}})
		{
			if(!transform.Has(std::string(vectorMember))) continue;
			const auto& vector = transform.Get(std::string(vectorMember));
			if(!vector.IsArray()
				|| vector.Get<tinygltf::Value::Array>().size() != 2u
				|| !std::all_of(vector.Get<tinygltf::Value::Array>().begin(),
				               vector.Get<tinygltf::Value::Array>().end(),
				               [](const tinygltf::Value& value) { return value.IsNumber(); }))
			{
				return fail(GltfValidationErrorCode::invalidValue,
				            transformPath + "." + std::string(vectorMember),
				            "texture transform vector must contain exactly two numbers");
			}
		}
		if(transform.Has("rotation") && !transform.Get("rotation").IsNumber())
		{
			return fail(GltfValidationErrorCode::invalidValue,
			            transformPath + ".rotation",
			            "texture transform rotation must be a number");
		}
		if(transform.Has("texCoord"))
		{
			const auto& texCoord = transform.Get("texCoord");
			if(!texCoord.IsInt() || texCoord.GetNumberAsInt() < 0)
			{
				return fail(GltfValidationErrorCode::invalidValue,
				            transformPath + ".texCoord",
				            "texture transform coordinate set must be a nonnegative integer");
			}
		}
		return true;
	}

	[[nodiscard]] bool validateNumberMember(
		const tinygltf::Value& extension,
		std::string_view member,
		const std::string& path,
		std::optional<double> minimum = std::nullopt,
		std::optional<double> maximum = std::nullopt,
		bool exclusiveMinimum = false,
		bool exclusiveMaximum = false)
	{
		if(!extension.Has(std::string(member))) return true;
		const auto& source = extension.Get(std::string(member));
		const std::string memberPath = path + "." + std::string(member);
		if(!source.IsNumber())
		{
			return fail(GltfValidationErrorCode::invalidValue,
			            memberPath,
			            "material extension member must be a number");
		}
		const double value = source.GetNumberAsDouble();
		const bool belowMinimum = minimum
			&& (exclusiveMinimum ? value <= *minimum : value < *minimum);
		const bool aboveMaximum = maximum
			&& (exclusiveMaximum ? value >= *maximum : value > *maximum);
		if(belowMinimum || aboveMaximum)
		{
			return fail(GltfValidationErrorCode::invalidValue,
			            memberPath,
			            "material extension number is outside its schema range");
		}
		return true;
	}

	[[nodiscard]] bool validateNumberArrayMember(
		const tinygltf::Value& extension,
		std::string_view member,
		std::size_t count,
		const std::string& path,
		std::optional<double> minimum = std::nullopt,
		std::optional<double> maximum = std::nullopt)
	{
		if(!extension.Has(std::string(member))) return true;
		const auto& source = extension.Get(std::string(member));
		const std::string memberPath = path + "." + std::string(member);
		if(!source.IsArray() || source.Get<tinygltf::Value::Array>().size() != count)
		{
			return fail(GltfValidationErrorCode::invalidValue,
			            memberPath,
			            "material extension vector has the wrong type or length");
		}
		const auto& values = source.Get<tinygltf::Value::Array>();
		for(std::size_t index = 0; index < values.size(); ++index)
		{
			if(!values[index].IsNumber())
			{
				return fail(GltfValidationErrorCode::invalidValue,
				            memberPath + "[" + std::to_string(index) + "]",
				            "material extension vector component must be a number");
			}
			const double value = values[index].GetNumberAsDouble();
			if((minimum && value < *minimum) || (maximum && value > *maximum))
			{
				return fail(GltfValidationErrorCode::invalidValue,
				            memberPath + "[" + std::to_string(index) + "]",
				            "material extension vector component is outside its schema range");
			}
		}
		return true;
	}

	[[nodiscard]] bool validateKnownMaterialExtension(
		const tinygltf::Value& extension,
		std::string_view name,
		const std::string& path)
	{
		if(!extension.IsObject())
		{
			return fail(GltfValidationErrorCode::invalidValue,
			            path,
			            "known material extension must be an object");
		}
		if(extension.Has("extensions") && !extension.Get("extensions").IsObject())
		{
			return fail(GltfValidationErrorCode::invalidValue,
			            path + ".extensions",
			            "nested extensions must be an object");
		}

		if(name == "KHR_materials_anisotropy")
		{
			return validateNumberMember(extension, "anisotropyStrength", path, 0.0, 1.0)
				&& validateNumberMember(
					extension, "anisotropyRotation", path,
					-static_cast<double>(3.14f), static_cast<double>(3.14f))
				&& validateTextureInfoExtension(extension, "anisotropyTexture", path);
		}
		if(name == "KHR_materials_clearcoat")
		{
			return validateNumberMember(extension, "clearcoatFactor", path, 0.0, 1.0)
				&& validateNumberMember(extension, "clearcoatRoughnessFactor", path, 0.0, 1.0)
				&& validateTextureInfoExtension(extension, "clearcoatTexture", path)
				&& validateTextureInfoExtension(extension, "clearcoatRoughnessTexture", path)
				&& validateTextureInfoExtension(extension, "clearcoatNormalTexture", path, true);
		}
		if(name == "KHR_materials_diffuse_transmission")
		{
			return validateNumberMember(extension, "diffuseTransmissionFactor", path, 0.0, 1.0)
				&& validateNumberArrayMember(extension, "diffuseTransmissionColorFactor", 3u, path, 0.0, 1.0)
				&& validateTextureInfoExtension(extension, "diffuseTransmissionTexture", path)
				&& validateTextureInfoExtension(extension, "diffuseTransmissionColorTexture", path);
		}
		if(name == "KHR_materials_dispersion")
		{
			return validateNumberMember(extension, "dispersion", path, 0.0, 10.0);
		}
		if(name == "KHR_materials_emissive_strength")
		{
			return validateNumberMember(
				extension, "emissiveStrength", path, 0.0,
				static_cast<double>(std::numeric_limits<float>::max()));
		}
		if(name == "KHR_materials_ior")
		{
			if(!validateNumberMember(extension, "ior", path, 0.0, 10.0)) return false;
			if(extension.Has("ior"))
			{
				const double ior = extension.Get("ior").GetNumberAsDouble();
				if(ior != 0.0 && ior < 1.0)
				{
					return fail(GltfValidationErrorCode::invalidValue,
					            path + ".ior",
					            "material IOR must be zero or at least one");
				}
			}
			return true;
		}
		if(name == "KHR_materials_iridescence")
		{
			if(!(validateNumberMember(extension, "iridescenceFactor", path, 0.0, 1.0)
				&& validateNumberMember(extension, "iridescenceIor", path, 1.0, 10.0)
				&& validateNumberMember(extension, "iridescenceThicknessMinimum", path, 0.0, 1000.0)
				&& validateNumberMember(extension, "iridescenceThicknessMaximum", path, 0.0, 1000.0)
				&& validateTextureInfoExtension(extension, "iridescenceTexture", path)
				&& validateTextureInfoExtension(extension, "iridescenceThicknessTexture", path)))
			{
				return false;
			}
			if(extension.Has("iridescenceThicknessMinimum")
				&& extension.Has("iridescenceThicknessMaximum")
				&& extension.Get("iridescenceThicknessMaximum").GetNumberAsDouble()
					< extension.Get("iridescenceThicknessMinimum").GetNumberAsDouble())
			{
				return fail(GltfValidationErrorCode::invalidValue,
				            path + ".iridescenceThicknessMaximum",
				            "iridescence maximum thickness must not be below its minimum");
			}
			return true;
		}
		if(name == "KHR_materials_sheen")
		{
			return validateNumberArrayMember(extension, "sheenColorFactor", 3u, path, 0.0, 1.0)
				&& validateNumberMember(extension, "sheenRoughnessFactor", path, 0.0, 1.0)
				&& validateTextureInfoExtension(extension, "sheenColorTexture", path)
				&& validateTextureInfoExtension(extension, "sheenRoughnessTexture", path);
		}
		if(name == "KHR_materials_specular")
		{
			return validateNumberMember(extension, "specularFactor", path, 0.0, 1.0)
				&& validateNumberArrayMember(extension, "specularColorFactor", 3u, path, 0.0)
				&& validateTextureInfoExtension(extension, "specularTexture", path)
				&& validateTextureInfoExtension(extension, "specularColorTexture", path);
		}
		if(name == "KHR_materials_transmission")
		{
			return validateNumberMember(extension, "transmissionFactor", path, 0.0, 1.0)
				&& validateTextureInfoExtension(extension, "transmissionTexture", path);
		}
		if(name == "KHR_materials_unlit") return true;
		if(name == "KHR_materials_volume")
		{
			return validateNumberMember(extension, "thicknessFactor", path, 0.0, 1.0)
				&& validateNumberMember(
					extension, "attenuationDistance", path, 0.0,
					static_cast<double>(std::numeric_limits<float>::max()), true)
				&& validateNumberArrayMember(extension, "attenuationColor", 3u, path, 0.0, 1.0)
				&& validateTextureInfoExtension(extension, "thicknessTexture", path);
		}
		if(name == "KHR_materials_volume_scatter")
		{
			return validateNumberArrayMember(extension, "multiscatterColor", 3u, path, 0.0, 1.0)
				&& validateNumberMember(extension, "scatterAnisotropy", path, -1.0, 1.0);
		}
		if(name == "KHR_materials_pbrSpecularGlossiness")
		{
			return validateNumberArrayMember(extension, "diffuseFactor", 4u, path, 0.0, 1.0)
				&& validateNumberArrayMember(extension, "specularFactor", 3u, path, 0.0, 1.0)
				&& validateNumberMember(extension, "glossinessFactor", path, 0.0, 1.0)
				&& validateTextureInfoExtension(extension, "diffuseTexture", path)
				&& validateTextureInfoExtension(extension, "specularGlossinessTexture", path);
		}
		return true;
	}

	[[nodiscard]] bool validateMaterialExtensionCompatibility(
		const tinygltf::Material& material,
		std::size_t materialIndex)
	{
		const std::string base = "materials[" + std::to_string(materialIndex) + "].extensions.";
		const bool specularGlossiness = material.extensions.contains("KHR_materials_pbrSpecularGlossiness");
		const bool unlit = material.extensions.contains("KHR_materials_unlit");
		if(unlit && specularGlossiness)
		{
			return fail(GltfValidationErrorCode::invalidValue,
			            base + "KHR_materials_pbrSpecularGlossiness",
			            "specular-glossiness and unlit material workflows are mutually exclusive");
		}
		static constexpr std::array incompatibleLayerExtensions{
			std::string_view{"KHR_materials_anisotropy"},
			std::string_view{"KHR_materials_clearcoat"},
			std::string_view{"KHR_materials_diffuse_transmission"},
			std::string_view{"KHR_materials_dispersion"},
			std::string_view{"KHR_materials_ior"},
			std::string_view{"KHR_materials_iridescence"},
			std::string_view{"KHR_materials_sheen"},
			std::string_view{"KHR_materials_specular"},
			std::string_view{"KHR_materials_transmission"},
			std::string_view{"KHR_materials_volume"},
			std::string_view{"KHR_materials_volume_scatter"},
		};
		for(const std::string_view name : incompatibleLayerExtensions)
		{
			if(!material.extensions.contains(std::string(name))) continue;
			if(specularGlossiness)
			{
				return fail(GltfValidationErrorCode::invalidValue,
				            base + std::string(name),
				            "material extension cannot be combined with the specular-glossiness workflow");
			}
			if(unlit)
			{
				return fail(GltfValidationErrorCode::invalidValue,
				            base + std::string(name),
				            "material extension cannot be combined with unlit");
			}
		}
		if(unlit && material.extensions.contains("KHR_materials_emissive_strength"))
		{
			return fail(GltfValidationErrorCode::invalidValue,
			            base + "KHR_materials_emissive_strength",
			            "emissive strength cannot be combined with unlit");
		}
		const bool volume = material.extensions.contains("KHR_materials_volume");
		if(material.extensions.contains("KHR_materials_dispersion") && !volume)
		{
			return fail(GltfValidationErrorCode::invalidValue,
			            base + "KHR_materials_dispersion",
			            "dispersion requires KHR_materials_volume");
		}
		if(material.extensions.contains("KHR_materials_volume_scatter") && !volume)
		{
			return fail(GltfValidationErrorCode::invalidValue,
			            base + "KHR_materials_volume_scatter",
			            "volume scattering requires KHR_materials_volume");
		}
		if(volume
			&& !material.extensions.contains("KHR_materials_transmission")
			&& !material.extensions.contains("KHR_materials_diffuse_transmission"))
		{
			return fail(GltfValidationErrorCode::invalidValue,
			            base + "KHR_materials_volume",
			            "volume requires a known transmission extension");
		}
		return true;
	}

	[[nodiscard]] bool validateKnownExtensionReferences()
	{
		static constexpr std::array<std::string_view, 4> alternateTextureSources{
			"KHR_texture_basisu",
			"EXT_texture_webp",
			"EXT_texture_avif",
			"MSFT_texture_dds",
		};
		for(std::size_t textureIndex = 0; textureIndex < m_model.textures.size(); ++textureIndex)
		{
			const auto& texture = m_model.textures[textureIndex];
			for(const std::string_view name : alternateTextureSources)
			{
				const auto found = texture.extensions.find(std::string(name));
				if(found == texture.extensions.end()) continue;
				const std::string path = "textures[" + std::to_string(textureIndex)
					+ "].extensions." + std::string(name);
				if(!found->second.IsObject() || !found->second.Has("source"))
				{
					return fail(GltfValidationErrorCode::invalidReference,
					            path,
					            "alternate texture source extension requires source");
				}
				if(!validateIntegerReference(found->second.Get("source"), m_model.images.size(), path + ".source"))
				{
					return false;
				}
			}
		}

		for(std::size_t viewIndex = 0; viewIndex < m_model.bufferViews.size(); ++viewIndex)
		{
			const auto compression = m_model.bufferViews[viewIndex].extensions.find("EXT_meshopt_compression");
			if(compression == m_model.bufferViews[viewIndex].extensions.end()) continue;
			const std::string path = "bufferViews[" + std::to_string(viewIndex)
				+ "].extensions.EXT_meshopt_compression";
			if(!compression->second.IsObject() || !compression->second.Has("buffer")
				|| !compression->second.Has("byteLength") || !compression->second.Has("byteStride")
				|| !compression->second.Has("count"))
			{
				return fail(GltfValidationErrorCode::invalidReference,
				            path,
				            "meshopt compression requires buffer, byteLength, byteStride, and count");
			}
			if(!validateIntegerReference(compression->second.Get("buffer"), m_model.buffers.size(), path + ".buffer"))
			{
				return false;
			}
			for(const std::string_view member : {std::string_view{"byteLength"}, std::string_view{"byteStride"}, std::string_view{"count"}})
			{
				const auto& value = compression->second.Get(std::string(member));
				if(!value.IsInt() || value.GetNumberAsInt() <= 0)
				{
					return fail(GltfValidationErrorCode::invalidBufferRange,
					            path + "." + std::string(member),
					            "meshopt compression range values must be positive integers");
				}
			}
			if(compression->second.Has("byteOffset"))
			{
				const auto& value = compression->second.Get("byteOffset");
				if(!value.IsInt() || value.GetNumberAsInt() < 0)
				{
					return fail(GltfValidationErrorCode::invalidBufferRange,
					            path + ".byteOffset",
					            "meshopt compression byteOffset must be a nonnegative integer");
				}
			}
		}

		std::size_t variantCount = 0;
		const auto variantsExtension = m_model.extensions.find("KHR_materials_variants");
		if(variantsExtension != m_model.extensions.end())
		{
			if(!variantsExtension->second.IsObject() || !variantsExtension->second.Has("variants")
				|| !variantsExtension->second.Get("variants").IsArray())
			{
				return fail(GltfValidationErrorCode::invalidReference,
				            "extensions.KHR_materials_variants.variants",
				            "materials variants extension requires a variants array");
			}
			variantCount = variantsExtension->second.Get("variants").Get<tinygltf::Value::Array>().size();
		}

		for(std::size_t meshIndex = 0; meshIndex < m_model.meshes.size(); ++meshIndex)
		{
			for(std::size_t primitiveIndex = 0;
				primitiveIndex < m_model.meshes[meshIndex].primitives.size();
				++primitiveIndex)
			{
				const auto& primitive = m_model.meshes[meshIndex].primitives[primitiveIndex];
				const std::string base = "meshes[" + std::to_string(meshIndex) + "].primitives["
					+ std::to_string(primitiveIndex) + "].extensions.";

				const auto draco = primitive.extensions.find("KHR_draco_mesh_compression");
				if(draco != primitive.extensions.end())
				{
					const std::string path = base + "KHR_draco_mesh_compression";
					if(!draco->second.IsObject() || !draco->second.Has("bufferView")
						|| !draco->second.Has("attributes") || !draco->second.Get("attributes").IsObject())
					{
						return fail(GltfValidationErrorCode::invalidReference,
						            path,
						            "Draco extension requires bufferView and attributes");
					}
					if(!validateIntegerReference(draco->second.Get("bufferView"), m_model.bufferViews.size(), path + ".bufferView"))
					{
						return false;
					}
					for(const auto& [semantic, attribute] : draco->second.Get("attributes").Get<tinygltf::Value::Object>())
					{
						if(semantic.empty() || !attribute.IsInt() || attribute.GetNumberAsInt() < 0)
						{
							return fail(GltfValidationErrorCode::invalidReference,
							            path + ".attributes." + semantic,
							            "Draco attribute identifiers must be nonnegative integers");
						}
					}
				}

				const auto variants = primitive.extensions.find("KHR_materials_variants");
				if(variants != primitive.extensions.end())
				{
					const std::string path = base + "KHR_materials_variants";
					if(variantCount == 0 || !variants->second.IsObject()
						|| !variants->second.Has("mappings") || !variants->second.Get("mappings").IsArray())
					{
						return fail(GltfValidationErrorCode::invalidReference,
						            path,
						            "primitive variants require the root variants array and mappings");
					}
					const auto& mappings = variants->second.Get("mappings").Get<tinygltf::Value::Array>();
					for(std::size_t mappingIndex = 0; mappingIndex < mappings.size(); ++mappingIndex)
					{
						const auto& mapping = mappings[mappingIndex];
						const std::string mappingPath = path + ".mappings[" + std::to_string(mappingIndex) + "]";
						if(!mapping.IsObject() || !mapping.Has("material") || !mapping.Has("variants")
							|| !mapping.Get("variants").IsArray())
						{
							return fail(GltfValidationErrorCode::invalidReference,
							            mappingPath,
							            "variant mapping requires material and variants");
						}
						if(!validateIntegerReference(mapping.Get("material"), m_model.materials.size(), mappingPath + ".material"))
						{
							return false;
						}
						for(std::size_t variantIndex = 0;
							variantIndex < mapping.Get("variants").Get<tinygltf::Value::Array>().size();
							++variantIndex)
						{
							if(!validateIntegerReference(
								mapping.Get("variants").Get<tinygltf::Value::Array>()[variantIndex],
								variantCount,
								mappingPath + ".variants[" + std::to_string(variantIndex) + "]"))
							{
								return false;
							}
						}
					}
				}
			}
		}

		for(std::size_t nodeIndex = 0; nodeIndex < m_model.nodes.size(); ++nodeIndex)
		{
			const auto instancing = m_model.nodes[nodeIndex].extensions.find("EXT_mesh_gpu_instancing");
			if(instancing == m_model.nodes[nodeIndex].extensions.end()) continue;
			const std::string path = "nodes[" + std::to_string(nodeIndex)
				+ "].extensions.EXT_mesh_gpu_instancing.attributes";
			if(!instancing->second.IsObject() || !instancing->second.Has("attributes")
				|| !instancing->second.Get("attributes").IsObject())
			{
				return fail(GltfValidationErrorCode::invalidReference,
				            path,
				            "GPU instancing extension requires attributes");
			}
			for(const auto& [semantic, accessor] : instancing->second.Get("attributes").Get<tinygltf::Value::Object>())
			{
				if(semantic.empty()
					|| !validateIntegerReference(accessor, m_model.accessors.size(), path + "." + semantic))
				{
					return false;
				}
			}
		}

		static constexpr std::array lockedMaterialExtensions{
			std::string_view{"KHR_materials_anisotropy"},
			std::string_view{"KHR_materials_clearcoat"},
			std::string_view{"KHR_materials_diffuse_transmission"},
			std::string_view{"KHR_materials_dispersion"},
			std::string_view{"KHR_materials_emissive_strength"},
			std::string_view{"KHR_materials_ior"},
			std::string_view{"KHR_materials_iridescence"},
			std::string_view{"KHR_materials_sheen"},
			std::string_view{"KHR_materials_specular"},
			std::string_view{"KHR_materials_transmission"},
			std::string_view{"KHR_materials_unlit"},
			std::string_view{"KHR_materials_volume"},
			std::string_view{"KHR_materials_volume_scatter"},
			std::string_view{"KHR_materials_pbrSpecularGlossiness"},
		};
		for(std::size_t materialIndex = 0; materialIndex < m_model.materials.size(); ++materialIndex)
		{
			const auto& material = m_model.materials[materialIndex];
			for(const std::string_view name : lockedMaterialExtensions)
			{
				const auto found = material.extensions.find(std::string(name));
				if(found == material.extensions.end()) continue;
				const std::string path = "materials[" + std::to_string(materialIndex)
					+ "].extensions." + std::string(name);
				if(!validateKnownMaterialExtension(found->second, name, path)) return false;
			}
			if(!validateMaterialExtensionCompatibility(material, materialIndex)) return false;

			const auto bump = material.extensions.find("EXT_materials_bump");
			if(bump != material.extensions.end()
				&& !validateTextureInfoExtension(
					bump->second,
					"bumpTexture",
					"materials[" + std::to_string(materialIndex) + "].extensions.EXT_materials_bump"))
			{
				return false;
			}
		}
		return true;
	}

	const tinygltf::Model& m_model;
	const EditorLimits& m_limits;
	GltfValidationReport m_report;
	std::size_t m_jsonValueCount{0};
	std::unordered_set<std::string> m_declaredExtensions;
	std::vector<AccessorRequirements> m_accessorRequirements;
};

} // namespace detail

bool GltfValidationReport::valid() const noexcept
{
	return m_valid;
}

const std::vector<GltfValidationIssue>& GltfValidationReport::issues() const noexcept
{
	return m_issues;
}

GltfValidationReport GltfValidator::validate(const GltfDocument& document, const EditorLimits& limits)
{
	GltfValidationReport report = validateModel(document.authorityModel(), limits);
	if(!report.valid())
	{
		return report;
	}
	if(document.dependencies().size() > limits.maxDependencies)
	{
		report.m_valid = false;
		report.m_issues.push_back(GltfValidationIssue{
			GltfValidationErrorCode::limitExceeded,
			"dependencies",
			"dependency count exceeds configured limit",
		});
	}
	return report;
}

GltfValidationReport GltfValidator::validateModel(const tinygltf::Model& model, const EditorLimits& limits)
{
	return detail::GltfModelValidator(model, limits).run();
}

} // namespace demo::editor
