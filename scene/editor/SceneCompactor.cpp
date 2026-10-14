#include <mgif/scene_editor/SceneCompactor.h>

#include <mgif/scene_editor/EditorFaultInjection.h>
#include <mgif/scene_editor/GltfDocument.h>
#include <mgif/scene_editor/GltfValidator.h>

#include <tiny_gltf.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>

namespace demo::editor {
namespace detail {
class SceneCompactorAccess final
{
public:
	[[nodiscard]] static const tinygltf::Model& model(const GltfDocument& document) noexcept
	{
		return document.authorityModel();
	}

	[[nodiscard]] static GltfValidationReport validateCandidate(std::unique_ptr<tinygltf::Model>& candidate,
																const EditorLimits& limits)
	{
		GltfDocument temporary(std::move(candidate), {}, GltfDocumentFormat::ascii, {});
		GltfValidationReport report = GltfValidator::validate(temporary, limits);
		candidate = temporary.takeAuthorityModel();
		return report;
	}

	[[nodiscard]] static MutationCommitStatus
	commit(GltfDocument& document, std::unique_ptr<tinygltf::Model> candidate, MutationRegistryPlan& plan,
		   MutationPrePublishCallback prePublish, void* context) noexcept
	{
		return document.commitMutation(std::move(candidate), plan, prePublish, context);
	}
};
} // namespace detail

namespace {

using CollectionKind = SceneCompactionCollectionKind;
using CollectionResult = SceneCompactionCollectionResult;

constexpr std::size_t collectionIndex(CollectionKind kind) noexcept
{
	return static_cast<std::size_t>(kind);
}

constexpr std::size_t objectKindIndex(EditorObjectKind kind) noexcept
{
	return static_cast<std::size_t>(kind);
}

void setMessage(SceneCompactionResult& result, std::string_view message) noexcept
{
	try
	{
		result.message.assign(message);
	}
	catch (...)
	{
		result.message.clear();
	}
}

[[nodiscard]] bool hasLimitIssue(const GltfValidationReport& report) noexcept
{
	return std::any_of(report.issues().begin(), report.issues().end(), [](const GltfValidationIssue& issue) {
		return issue.code == GltfValidationErrorCode::limitExceeded;
	});
}

[[nodiscard]] std::string_view firstIssueMessage(const GltfValidationReport& report,
												 std::string_view fallback) noexcept
{
	return report.issues().empty() ? fallback : std::string_view{report.issues().front().message};
}

void setBeforeCounts(SceneCompactionResult& result, const tinygltf::Model& model) noexcept
{
	result.collections[collectionIndex(CollectionKind::scene)].beforeCount = model.scenes.size();
	result.collections[collectionIndex(CollectionKind::node)].beforeCount = model.nodes.size();
	result.collections[collectionIndex(CollectionKind::mesh)].beforeCount = model.meshes.size();
	result.collections[collectionIndex(CollectionKind::material)].beforeCount = model.materials.size();
	result.collections[collectionIndex(CollectionKind::texture)].beforeCount = model.textures.size();
	result.collections[collectionIndex(CollectionKind::image)].beforeCount = model.images.size();
	result.collections[collectionIndex(CollectionKind::sampler)].beforeCount = model.samplers.size();
	result.collections[collectionIndex(CollectionKind::camera)].beforeCount = model.cameras.size();
	result.collections[collectionIndex(CollectionKind::light)].beforeCount = model.lights.size();
	result.collections[collectionIndex(CollectionKind::accessor)].beforeCount = model.accessors.size();
	result.collections[collectionIndex(CollectionKind::bufferView)].beforeCount = model.bufferViews.size();
	result.collections[collectionIndex(CollectionKind::buffer)].beforeCount = model.buffers.size();
	result.collections[collectionIndex(CollectionKind::skin)].beforeCount = model.skins.size();
	result.collections[collectionIndex(CollectionKind::animation)].beforeCount = model.animations.size();
	result.collections[collectionIndex(CollectionKind::audioEmitter)].beforeCount =
		model.audioEmitters.size();
	result.collections[collectionIndex(CollectionKind::audioSource)].beforeCount = model.audioSources.size();
	std::size_t primitiveCount = 0;
	for (const tinygltf::Mesh& mesh : model.meshes)
		primitiveCount += mesh.primitives.size();
	result.collections[collectionIndex(CollectionKind::primitive)].beforeCount = primitiveCount;
}

enum class ReachKind : std::uint8_t
{
	node,
	mesh,
	material,
	texture,
	image,
	sampler,
	camera,
	light,
	accessor,
	bufferView,
	buffer,
	skin,
	audioEmitter,
	audioSource,
};

struct WorkItem final
{
	ReachKind kind;
	std::size_t index;
};

struct Reachability final
{
	std::vector<bool> scenes;
	std::vector<bool> nodes;
	std::vector<bool> meshes;
	std::vector<bool> materials;
	std::vector<bool> textures;
	std::vector<bool> images;
	std::vector<bool> samplers;
	std::vector<bool> cameras;
	std::vector<bool> lights;
	std::vector<bool> accessors;
	std::vector<bool> bufferViews;
	std::vector<bool> buffers;
	std::vector<bool> skins;
	std::vector<bool> animations;
	std::vector<bool> audioEmitters;
	std::vector<bool> audioSources;
};

class ReachabilityBuilder final
{
public:
	explicit ReachabilityBuilder(const tinygltf::Model& model) : m_model(model)
	{
		m_reach.scenes.assign(model.scenes.size(), true);
		m_reach.nodes.assign(model.nodes.size(), false);
		m_reach.meshes.assign(model.meshes.size(), false);
		m_reach.materials.assign(model.materials.size(), false);
		m_reach.textures.assign(model.textures.size(), false);
		m_reach.images.assign(model.images.size(), false);
		m_reach.samplers.assign(model.samplers.size(), false);
		m_reach.cameras.assign(model.cameras.size(), false);
		m_reach.lights.assign(model.lights.size(), false);
		m_reach.accessors.assign(model.accessors.size(), false);
		m_reach.bufferViews.assign(model.bufferViews.size(), false);
		m_reach.buffers.assign(model.buffers.size(), false);
		m_reach.skins.assign(model.skins.size(), false);
		m_reach.animations.assign(model.animations.size(), true);
		m_reach.audioEmitters.assign(model.audioEmitters.size(), false);
		m_reach.audioSources.assign(model.audioSources.size(), false);
	}

	[[nodiscard]] bool build(Reachability& output)
	{
		m_work.reserve(m_model.nodes.size() + m_model.meshes.size() + m_model.materials.size() +
					   m_model.accessors.size());
		for (const tinygltf::Scene& scene : m_model.scenes)
		{
			for (const int node : scene.nodes)
				mark(ReachKind::node, node);
			for (const int emitter : scene.audioEmitters)
				mark(ReachKind::audioEmitter, emitter);
		}

		// Top-level animations and skins are independent semantic roots in glTF.
		// Keeping them also keeps their node and accessor dependencies.
		for (std::size_t index = 0; index < m_model.animations.size(); ++index)
		{
			const tinygltf::Animation& animation = m_model.animations[index];
			for (const tinygltf::AnimationSampler& sampler : animation.samplers)
			{
				mark(ReachKind::accessor, sampler.input);
				mark(ReachKind::accessor, sampler.output);
			}
			for (const tinygltf::AnimationChannel& channel : animation.channels)
				mark(ReachKind::node, channel.target_node, true);
		}
		for (std::size_t index = 0; index < m_model.skins.size(); ++index)
			mark(ReachKind::skin, static_cast<int>(index));

		while (m_valid && !m_work.empty())
		{
			const WorkItem item = m_work.back();
			m_work.pop_back();
			visit(item);
		}
		if (!m_valid)
			return false;
		output = std::move(m_reach);
		return true;
	}

private:
	[[nodiscard]] std::vector<bool>& marks(ReachKind kind) noexcept
	{
		switch (kind)
		{
		case ReachKind::node:
			return m_reach.nodes;
		case ReachKind::mesh:
			return m_reach.meshes;
		case ReachKind::material:
			return m_reach.materials;
		case ReachKind::texture:
			return m_reach.textures;
		case ReachKind::image:
			return m_reach.images;
		case ReachKind::sampler:
			return m_reach.samplers;
		case ReachKind::camera:
			return m_reach.cameras;
		case ReachKind::light:
			return m_reach.lights;
		case ReachKind::accessor:
			return m_reach.accessors;
		case ReachKind::bufferView:
			return m_reach.bufferViews;
		case ReachKind::buffer:
			return m_reach.buffers;
		case ReachKind::skin:
			return m_reach.skins;
		case ReachKind::audioEmitter:
			return m_reach.audioEmitters;
		case ReachKind::audioSource:
			return m_reach.audioSources;
		}
		return m_reach.nodes;
	}

	void mark(ReachKind kind, int index, bool optional = false)
	{
		if (index < 0)
		{
			if (!optional)
				m_valid = false;
			return;
		}
		auto& values = marks(kind);
		if (static_cast<std::size_t>(index) >= values.size())
		{
			m_valid = false;
			return;
		}
		if (values[static_cast<std::size_t>(index)])
			return;
		values[static_cast<std::size_t>(index)] = true;
		m_work.push_back(WorkItem{kind, static_cast<std::size_t>(index)});
	}

	void markValue(ReachKind kind, const tinygltf::Value& value)
	{
		if (!value.IsInt())
		{
			m_valid = false;
			return;
		}
		mark(kind, value.GetNumberAsInt());
	}

	void materialTexture(int index)
	{
		mark(ReachKind::texture, index, true);
	}

	void extensionTexture(const tinygltf::Material& material, std::string_view extensionName,
						  std::string_view member)
	{
		const auto found = material.extensions.find(std::string(extensionName));
		if (found == material.extensions.end())
			return;
		if (!found->second.IsObject())
		{
			m_valid = false;
			return;
		}
		const auto& object = found->second.Get<tinygltf::Value::Object>();
		const auto texture = object.find(std::string(member));
		if (texture == object.end())
			return;
		if (!texture->second.IsObject() || !texture->second.Has("index"))
		{
			m_valid = false;
			return;
		}
		markValue(ReachKind::texture, texture->second.Get("index"));
	}

	void visitMaterial(const tinygltf::Material& material)
	{
		for (const int lod : material.lods)
			mark(ReachKind::material, lod);
		materialTexture(material.pbrMetallicRoughness.baseColorTexture.index);
		materialTexture(material.pbrMetallicRoughness.metallicRoughnessTexture.index);
		materialTexture(material.normalTexture.index);
		materialTexture(material.occlusionTexture.index);
		materialTexture(material.emissiveTexture.index);
		for (const auto& [name, parameter] : material.values)
		{
			(void)name;
			const auto found = parameter.json_double_value.find("index");
			if (found != parameter.json_double_value.end())
			{
				const double value = found->second;
				if (value < 0.0 || value > static_cast<double>(std::numeric_limits<int>::max()) ||
					static_cast<double>(static_cast<int>(value)) != value)
					m_valid = false;
				else
					mark(ReachKind::texture, static_cast<int>(value));
			}
		}
		for (const auto& [name, parameter] : material.additionalValues)
		{
			(void)name;
			const auto found = parameter.json_double_value.find("index");
			if (found != parameter.json_double_value.end())
			{
				const double value = found->second;
				if (value < 0.0 || value > static_cast<double>(std::numeric_limits<int>::max()) ||
					static_cast<double>(static_cast<int>(value)) != value)
					m_valid = false;
				else
					mark(ReachKind::texture, static_cast<int>(value));
			}
		}
		extensionTexture(material, "KHR_materials_anisotropy", "anisotropyTexture");
		extensionTexture(material, "KHR_materials_clearcoat", "clearcoatTexture");
		extensionTexture(material, "KHR_materials_clearcoat", "clearcoatRoughnessTexture");
		extensionTexture(material, "KHR_materials_clearcoat", "clearcoatNormalTexture");
		extensionTexture(material, "KHR_materials_diffuse_transmission", "diffuseTransmissionTexture");
		extensionTexture(material, "KHR_materials_diffuse_transmission", "diffuseTransmissionColorTexture");
		extensionTexture(material, "KHR_materials_iridescence", "iridescenceTexture");
		extensionTexture(material, "KHR_materials_iridescence", "iridescenceThicknessTexture");
		extensionTexture(material, "KHR_materials_sheen", "sheenColorTexture");
		extensionTexture(material, "KHR_materials_sheen", "sheenRoughnessTexture");
		extensionTexture(material, "KHR_materials_specular", "specularTexture");
		extensionTexture(material, "KHR_materials_specular", "specularColorTexture");
		extensionTexture(material, "KHR_materials_transmission", "transmissionTexture");
		extensionTexture(material, "KHR_materials_volume", "thicknessTexture");
		extensionTexture(material, "KHR_materials_pbrSpecularGlossiness", "diffuseTexture");
		extensionTexture(material, "KHR_materials_pbrSpecularGlossiness", "specularGlossinessTexture");
		extensionTexture(material, "EXT_materials_bump", "bumpTexture");
	}

	void visit(WorkItem item)
	{
		switch (item.kind)
		{
		case ReachKind::node: {
			const tinygltf::Node& node = m_model.nodes[item.index];
			for (const int child : node.children)
				mark(ReachKind::node, child);
			for (const int lod : node.lods)
				mark(ReachKind::node, lod);
			mark(ReachKind::mesh, node.mesh, true);
			mark(ReachKind::skin, node.skin, true);
			mark(ReachKind::camera, node.camera, true);
			mark(ReachKind::light, node.light, true);
			mark(ReachKind::audioEmitter, node.emitter, true);
			const auto instancing = node.extensions.find("EXT_mesh_gpu_instancing");
			if (instancing != node.extensions.end())
			{
				if (!instancing->second.IsObject() || !instancing->second.Has("attributes") ||
					!instancing->second.Get("attributes").IsObject())
				{
					m_valid = false;
					break;
				}
				for (const auto& [semantic, accessor] :
					 instancing->second.Get("attributes").Get<tinygltf::Value::Object>())
				{
					(void)semantic;
					markValue(ReachKind::accessor, accessor);
				}
			}
			break;
		}
		case ReachKind::mesh:
			for (const tinygltf::Primitive& primitive : m_model.meshes[item.index].primitives)
			{
				mark(ReachKind::accessor, primitive.indices, true);
				for (const auto& [semantic, accessor] : primitive.attributes)
				{
					(void)semantic;
					mark(ReachKind::accessor, accessor);
				}
				for (const auto& target : primitive.targets)
					for (const auto& [semantic, accessor] : target)
					{
						(void)semantic;
						mark(ReachKind::accessor, accessor);
					}
				mark(ReachKind::material, primitive.material, true);
				const auto variants = primitive.extensions.find("KHR_materials_variants");
				if (variants != primitive.extensions.end())
				{
					if (!variants->second.IsObject() || !variants->second.Has("mappings") ||
						!variants->second.Get("mappings").IsArray())
					{
						m_valid = false;
						break;
					}
					for (const tinygltf::Value& mapping :
						 variants->second.Get("mappings").Get<tinygltf::Value::Array>())
					{
						if (!mapping.IsObject() || !mapping.Has("material"))
						{
							m_valid = false;
							break;
						}
						markValue(ReachKind::material, mapping.Get("material"));
					}
				}
				const auto draco = primitive.extensions.find("KHR_draco_mesh_compression");
				if (draco != primitive.extensions.end())
				{
					if (!draco->second.IsObject() || !draco->second.Has("bufferView"))
						m_valid = false;
					else
						markValue(ReachKind::bufferView, draco->second.Get("bufferView"));
				}
			}
			break;
		case ReachKind::material:
			visitMaterial(m_model.materials[item.index]);
			break;
		case ReachKind::texture: {
			const tinygltf::Texture& texture = m_model.textures[item.index];
			mark(ReachKind::sampler, texture.sampler, true);
			mark(ReachKind::image, texture.source, true);
			for (const std::string_view name :
				 {std::string_view{"KHR_texture_basisu"}, std::string_view{"EXT_texture_webp"},
				  std::string_view{"EXT_texture_avif"}, std::string_view{"MSFT_texture_dds"}})
			{
				const auto extension = texture.extensions.find(std::string(name));
				if (extension == texture.extensions.end())
					continue;
				if (!extension->second.IsObject() || !extension->second.Has("source"))
					m_valid = false;
				else
					markValue(ReachKind::image, extension->second.Get("source"));
			}
			break;
		}
		case ReachKind::image:
			mark(ReachKind::bufferView, m_model.images[item.index].bufferView, true);
			break;
		case ReachKind::sampler:
		case ReachKind::camera:
		case ReachKind::light:
		case ReachKind::buffer:
			break;
		case ReachKind::accessor: {
			const tinygltf::Accessor& accessor = m_model.accessors[item.index];
			mark(ReachKind::bufferView, accessor.bufferView, true);
			if (accessor.sparse.isSparse)
			{
				mark(ReachKind::bufferView, accessor.sparse.indices.bufferView);
				mark(ReachKind::bufferView, accessor.sparse.values.bufferView);
			}
			break;
		}
		case ReachKind::bufferView: {
			const tinygltf::BufferView& view = m_model.bufferViews[item.index];
			mark(ReachKind::buffer, view.buffer);
			const auto meshopt = view.extensions.find("EXT_meshopt_compression");
			if (meshopt != view.extensions.end())
			{
				if (!meshopt->second.IsObject() || !meshopt->second.Has("buffer"))
					m_valid = false;
				else
					markValue(ReachKind::buffer, meshopt->second.Get("buffer"));
			}
			break;
		}
		case ReachKind::skin: {
			const tinygltf::Skin& skin = m_model.skins[item.index];
			mark(ReachKind::accessor, skin.inverseBindMatrices, true);
			mark(ReachKind::node, skin.skeleton, true);
			for (const int joint : skin.joints)
				mark(ReachKind::node, joint);
			break;
		}
		case ReachKind::audioEmitter:
			mark(ReachKind::audioSource, m_model.audioEmitters[item.index].source);
			break;
		case ReachKind::audioSource:
			mark(ReachKind::bufferView, m_model.audioSources[item.index].bufferView, true);
			break;
		}
	}

	const tinygltf::Model& m_model;
	Reachability m_reach;
	std::vector<WorkItem> m_work;
	bool m_valid{true};
};

void buildMap(CollectionResult& result, const std::vector<bool>& retained)
{
	result.beforeCount = retained.size();
	result.oldToNew.resize(retained.size());
	std::size_t next = 0;
	for (std::size_t index = 0; index < retained.size(); ++index)
	{
		if (retained[index])
			result.oldToNew[index] = next++;
	}
	result.afterCount = next;
}

[[nodiscard]] const CollectionResult& collection(const SceneCompactionResult& result,
												 CollectionKind kind) noexcept
{
	return result.collections[collectionIndex(kind)];
}

[[nodiscard]] bool anyCollectionChanged(const SceneCompactionResult& result) noexcept
{
	return std::any_of(result.collections.begin(), result.collections.end(),
					   [](const CollectionResult& value) { return value.beforeCount != value.afterCount; });
}

void buildMaps(SceneCompactionResult& result, const tinygltf::Model& model, const Reachability& reach)
{
	buildMap(result.collections[collectionIndex(CollectionKind::scene)], reach.scenes);
	buildMap(result.collections[collectionIndex(CollectionKind::node)], reach.nodes);
	buildMap(result.collections[collectionIndex(CollectionKind::mesh)], reach.meshes);
	buildMap(result.collections[collectionIndex(CollectionKind::material)], reach.materials);
	buildMap(result.collections[collectionIndex(CollectionKind::texture)], reach.textures);
	buildMap(result.collections[collectionIndex(CollectionKind::image)], reach.images);
	buildMap(result.collections[collectionIndex(CollectionKind::sampler)], reach.samplers);
	buildMap(result.collections[collectionIndex(CollectionKind::camera)], reach.cameras);
	buildMap(result.collections[collectionIndex(CollectionKind::light)], reach.lights);
	buildMap(result.collections[collectionIndex(CollectionKind::accessor)], reach.accessors);
	buildMap(result.collections[collectionIndex(CollectionKind::bufferView)], reach.bufferViews);
	buildMap(result.collections[collectionIndex(CollectionKind::buffer)], reach.buffers);
	buildMap(result.collections[collectionIndex(CollectionKind::skin)], reach.skins);
	buildMap(result.collections[collectionIndex(CollectionKind::animation)], reach.animations);
	buildMap(result.collections[collectionIndex(CollectionKind::audioEmitter)], reach.audioEmitters);
	buildMap(result.collections[collectionIndex(CollectionKind::audioSource)], reach.audioSources);

	auto& primitives = result.collections[collectionIndex(CollectionKind::primitive)];
	primitives.oldToNew.resize(primitives.beforeCount);
	std::size_t oldFlat = 0;
	std::size_t newFlat = 0;
	for (std::size_t mesh = 0; mesh < model.meshes.size(); ++mesh)
	{
		for (std::size_t primitive = 0; primitive < model.meshes[mesh].primitives.size(); ++primitive)
		{
			if (reach.meshes[mesh])
				primitives.oldToNew[oldFlat] = newFlat++;
			++oldFlat;
		}
	}
	primitives.afterCount = newFlat;
}

[[nodiscard]] bool suspiciousKey(std::string_view key)
{
	std::string normalized;
	normalized.reserve(key.size());
	for (const unsigned char character : key)
		if (std::isalnum(character) != 0)
			normalized.push_back(static_cast<char>(std::tolower(character)));
	if (normalized.ends_with("index") || normalized.ends_with("indices"))
		return true;
	static constexpr std::array<std::string_view, 44> names{
		"index",	"indices",	   "ids",		   "source",   "scene",		"scenes",	  "node",
		"nodes",	"parent",	   "children",	   "mesh",	   "meshes",	"material",	  "materials",
		"texture",	"textures",	   "image",		   "images",   "sampler",	"samplers",	  "camera",
		"cameras",	"light",	   "lights",	   "accessor", "accessors", "bufferview", "bufferviews",
		"buffer",	"buffers",	   "skin",		   "skins",	   "animation", "animations", "emitter",
		"emitters", "audiosource", "audiosources", "skeleton", "joint",		"joints",	  "attributes",
		"targets",	"defaultscene"};
	return std::find(names.begin(), names.end(), normalized) != names.end();
}

[[nodiscard]] bool containsNonnegativeInteger(const tinygltf::Value& value)
{
	if (value.IsInt())
		return value.GetNumberAsInt() >= 0;
	if (value.IsArray())
		return std::any_of(value.Get<tinygltf::Value::Array>().begin(),
						   value.Get<tinygltf::Value::Array>().end(), containsNonnegativeInteger);
	return false;
}

[[nodiscard]] bool indexedPointerString(std::string_view value)
{
	static constexpr std::array<std::string_view, 16> collections{
		"scenes", "nodes",	   "meshes",	  "materials", "textures", "images",	 "samplers", "cameras",
		"lights", "accessors", "bufferViews", "buffers",   "skins",	   "animations", "emitters", "sources"};
	for (const std::string_view name : collections)
	{
		const std::string needle = "/" + std::string(name) + "/";
		const std::size_t position = value.find(needle);
		if (position == std::string_view::npos)
			continue;
		const std::size_t digit = position + needle.size();
		if (digit < value.size() && value[digit] >= '0' && value[digit] <= '9')
			return true;
	}
	return false;
}

[[nodiscard]] bool payloadLooksIndexed(const tinygltf::Value& value)
{
	if (value.IsString())
		return indexedPointerString(value.Get<std::string>());
	if (value.IsArray())
		return std::any_of(value.Get<tinygltf::Value::Array>().begin(),
						   value.Get<tinygltf::Value::Array>().end(), payloadLooksIndexed);
	if (!value.IsObject())
		return false;
	for (const auto& [key, member] : value.Get<tinygltf::Value::Object>())
	{
		if ((suspiciousKey(key) && containsNonnegativeInteger(member)) || payloadLooksIndexed(member))
			return true;
	}
	return false;
}

[[nodiscard]] bool knownName(std::string_view name, std::span<const std::string_view> names) noexcept
{
	return std::find(names.begin(), names.end(), name) != names.end();
}

[[nodiscard]] bool unsafeExtensionMap(const tinygltf::ExtensionMap& extensions,
									  std::span<const std::string_view> known, std::string& unsafeName)
{
	for (const auto& [name, value] : extensions)
	{
		if (knownName(name, known))
			continue;
		if (payloadLooksIndexed(value))
		{
			unsafeName = name;
			return true;
		}
	}
	return false;
}

template <typename Value, typename Visitor>
[[nodiscard]] bool visitRetained(const std::vector<Value>& values, const std::vector<bool>& retained,
								 Visitor&& visitor)
{
	for (std::size_t index = 0; index < values.size(); ++index)
		if (retained[index] && visitor(values[index]))
			return true;
	return false;
}

[[nodiscard]] bool hasUnsafeUnknownReference(const tinygltf::Model& model, const Reachability& reach,
											 std::string& unsafeName)
{
	static constexpr std::array rootKnown{std::string_view{"KHR_lights_punctual"},
										  std::string_view{"KHR_audio"},
										  std::string_view{"KHR_materials_variants"}};
	static constexpr std::array sceneKnown{std::string_view{"KHR_audio"}};
	static constexpr std::array nodeKnown{
		std::string_view{"KHR_lights_punctual"}, std::string_view{"KHR_audio"}, std::string_view{"MSFT_lod"},
		std::string_view{"EXT_mesh_gpu_instancing"}, std::string_view{"KHR_node_visibility"}};
	static constexpr std::array primitiveKnown{std::string_view{"KHR_draco_mesh_compression"},
											   std::string_view{"KHR_materials_variants"}};
	static constexpr std::array materialKnown{std::string_view{"MSFT_lod"},
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
											  std::string_view{"EXT_materials_bump"}};
	static constexpr std::array textureKnown{
		std::string_view{"KHR_texture_basisu"}, std::string_view{"EXT_texture_webp"},
		std::string_view{"EXT_texture_avif"}, std::string_view{"MSFT_texture_dds"}};
	static constexpr std::array textureInfoKnown{std::string_view{"KHR_texture_transform"}};
	static constexpr std::array viewKnown{std::string_view{"EXT_meshopt_compression"}};
	static constexpr std::array noKnown = std::array<std::string_view, 0>{};

	const auto scan = [&](const tinygltf::ExtensionMap& extensions, auto&& known) {
		return unsafeExtensionMap(extensions, std::span<const std::string_view>{known}, unsafeName);
	};
	if (scan(model.extensions, rootKnown) || scan(model.asset.extensions, noKnown))
		return true;
	if (visitRetained(model.scenes, reach.scenes,
					  [&](const tinygltf::Scene& value) { return scan(value.extensions, sceneKnown); }))
		return true;
	if (visitRetained(model.nodes, reach.nodes,
					  [&](const tinygltf::Node& value) { return scan(value.extensions, nodeKnown); }))
		return true;
	if (visitRetained(model.meshes, reach.meshes, [&](const tinygltf::Mesh& mesh) {
			if (scan(mesh.extensions, noKnown))
				return true;
			for (const tinygltf::Primitive& primitive : mesh.primitives)
				if (scan(primitive.extensions, primitiveKnown))
					return true;
			return false;
		}))
		return true;
	if (visitRetained(model.materials, reach.materials, [&](const tinygltf::Material& material) {
			if (scan(material.extensions, materialKnown) ||
				scan(material.pbrMetallicRoughness.extensions, noKnown) ||
				scan(material.pbrMetallicRoughness.baseColorTexture.extensions, textureInfoKnown) ||
				scan(material.pbrMetallicRoughness.metallicRoughnessTexture.extensions, textureInfoKnown) ||
				scan(material.normalTexture.extensions, textureInfoKnown) ||
				scan(material.occlusionTexture.extensions, textureInfoKnown) ||
				scan(material.emissiveTexture.extensions, textureInfoKnown))
				return true;
			return false;
		}))
		return true;
	if (visitRetained(model.textures, reach.textures,
					  [&](const tinygltf::Texture& value) { return scan(value.extensions, textureKnown); }))
		return true;
	if (visitRetained(model.images, reach.images,
					  [&](const tinygltf::Image& value) { return scan(value.extensions, noKnown); }))
		return true;
	if (visitRetained(model.samplers, reach.samplers,
					  [&](const tinygltf::Sampler& value) { return scan(value.extensions, noKnown); }))
		return true;
	if (visitRetained(model.cameras, reach.cameras, [&](const tinygltf::Camera& value) {
			return scan(value.extensions, noKnown) || scan(value.perspective.extensions, noKnown) ||
				   scan(value.orthographic.extensions, noKnown);
		}))
		return true;
	if (visitRetained(model.lights, reach.lights, [&](const tinygltf::Light& value) {
			return scan(value.extensions, noKnown) || scan(value.spot.extensions, noKnown);
		}))
		return true;
	if (visitRetained(model.accessors, reach.accessors, [&](const tinygltf::Accessor& value) {
			return scan(value.extensions, noKnown) || scan(value.sparse.extensions, noKnown) ||
				   scan(value.sparse.indices.extensions, noKnown) ||
				   scan(value.sparse.values.extensions, noKnown);
		}))
		return true;
	if (visitRetained(model.bufferViews, reach.bufferViews,
					  [&](const tinygltf::BufferView& value) { return scan(value.extensions, viewKnown); }))
		return true;
	if (visitRetained(model.buffers, reach.buffers,
					  [&](const tinygltf::Buffer& value) { return scan(value.extensions, noKnown); }))
		return true;
	if (visitRetained(model.skins, reach.skins,
					  [&](const tinygltf::Skin& value) { return scan(value.extensions, noKnown); }))
		return true;
	if (visitRetained(model.animations, reach.animations, [&](const tinygltf::Animation& animation) {
			if (scan(animation.extensions, noKnown))
				return true;
			for (const tinygltf::AnimationSampler& sampler : animation.samplers)
				if (scan(sampler.extensions, noKnown))
					return true;
			for (const tinygltf::AnimationChannel& channel : animation.channels)
				if (scan(channel.extensions, noKnown) || scan(channel.target_extensions, noKnown))
					return true;
			return false;
		}))
		return true;
	if (visitRetained(model.audioEmitters, reach.audioEmitters, [&](const tinygltf::AudioEmitter& value) {
			return scan(value.extensions, noKnown) || scan(value.positional.extensions, noKnown);
		}))
		return true;
	return visitRetained(model.audioSources, reach.audioSources,
						 [&](const tinygltf::AudioSource& value) { return scan(value.extensions, noKnown); });
}

template <typename T> void filterVector(std::vector<T>& values, const std::vector<bool>& retained)
{
	std::vector<T> filtered;
	filtered.reserve(static_cast<std::size_t>(std::count(retained.begin(), retained.end(), true)));
	for (std::size_t index = 0; index < values.size(); ++index)
		if (retained[index])
			filtered.push_back(std::move(values[index]));
	values.swap(filtered);
}

[[nodiscard]] bool remapIndex(int& value, const CollectionResult& mapping, bool optional = true)
{
	if (value < 0)
		return optional;
	const auto next = mapping.newIndex(static_cast<std::size_t>(value));
	if (!next || *next > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		return false;
	value = static_cast<int>(*next);
	return true;
}

[[nodiscard]] bool remapValue(tinygltf::Value& value, const CollectionResult& mapping)
{
	if (!value.IsInt())
		return false;
	int index = value.GetNumberAsInt();
	if (!remapIndex(index, mapping, false))
		return false;
	value = tinygltf::Value(index);
	return true;
}

[[nodiscard]] bool remapParameter(double& value, const CollectionResult& mapping)
{
	if (value < 0.0 || value > static_cast<double>(std::numeric_limits<int>::max()) ||
		static_cast<double>(static_cast<int>(value)) != value)
		return false;
	int index = static_cast<int>(value);
	if (!remapIndex(index, mapping, false))
		return false;
	value = static_cast<double>(index);
	return true;
}

[[nodiscard]] bool remapExtensionTexture(tinygltf::Material& material, std::string_view extensionName,
										 std::string_view member, const CollectionResult& textures)
{
	const auto found = material.extensions.find(std::string(extensionName));
	if (found == material.extensions.end())
		return true;
	if (!found->second.IsObject())
		return false;
	auto& object = found->second.Get<tinygltf::Value::Object>();
	const auto texture = object.find(std::string(member));
	if (texture == object.end())
		return true;
	if (!texture->second.IsObject() || !texture->second.Has("index"))
		return false;
	return remapValue(texture->second.Get<tinygltf::Value::Object>()["index"], textures);
}

[[nodiscard]] bool rewriteReferences(tinygltf::Model& model, const SceneCompactionResult& result)
{
	const auto& scenes = collection(result, CollectionKind::scene);
	const auto& nodes = collection(result, CollectionKind::node);
	const auto& meshes = collection(result, CollectionKind::mesh);
	const auto& materials = collection(result, CollectionKind::material);
	const auto& textures = collection(result, CollectionKind::texture);
	const auto& images = collection(result, CollectionKind::image);
	const auto& samplers = collection(result, CollectionKind::sampler);
	const auto& cameras = collection(result, CollectionKind::camera);
	const auto& lights = collection(result, CollectionKind::light);
	const auto& accessors = collection(result, CollectionKind::accessor);
	const auto& views = collection(result, CollectionKind::bufferView);
	const auto& buffers = collection(result, CollectionKind::buffer);
	const auto& skins = collection(result, CollectionKind::skin);
	const auto& emitters = collection(result, CollectionKind::audioEmitter);
	const auto& sources = collection(result, CollectionKind::audioSource);

	if (!remapIndex(model.defaultScene, scenes))
		return false;
	for (tinygltf::Scene& scene : model.scenes)
	{
		for (int& node : scene.nodes)
			if (!remapIndex(node, nodes, false))
				return false;
		for (int& emitter : scene.audioEmitters)
			if (!remapIndex(emitter, emitters, false))
				return false;
		const auto audio = scene.extensions.find("KHR_audio");
		if (audio != scene.extensions.end() && audio->second.IsObject() && audio->second.Has("emitters") &&
			audio->second.Get("emitters").IsArray())
			for (tinygltf::Value& value :
				 audio->second.Get<tinygltf::Value::Object>()["emitters"].Get<tinygltf::Value::Array>())
				if (!remapValue(value, emitters))
					return false;
	}
	for (tinygltf::Node& node : model.nodes)
	{
		for (int& child : node.children)
			if (!remapIndex(child, nodes, false))
				return false;
		for (int& lod : node.lods)
			if (!remapIndex(lod, nodes, false))
				return false;
		if (!remapIndex(node.mesh, meshes) || !remapIndex(node.skin, skins) ||
			!remapIndex(node.camera, cameras) || !remapIndex(node.light, lights) ||
			!remapIndex(node.emitter, emitters))
			return false;
		const auto light = node.extensions.find("KHR_lights_punctual");
		if (light != node.extensions.end() && light->second.IsObject() && light->second.Has("light") &&
			!remapValue(light->second.Get<tinygltf::Value::Object>()["light"], lights))
			return false;
		const auto audio = node.extensions.find("KHR_audio");
		if (audio != node.extensions.end() && audio->second.IsObject() && audio->second.Has("emitter") &&
			!remapValue(audio->second.Get<tinygltf::Value::Object>()["emitter"], emitters))
			return false;
		const auto lod = node.extensions.find("MSFT_lod");
		if (lod != node.extensions.end() && lod->second.IsObject() && lod->second.Has("ids") &&
			lod->second.Get("ids").IsArray())
			for (tinygltf::Value& value :
				 lod->second.Get<tinygltf::Value::Object>()["ids"].Get<tinygltf::Value::Array>())
				if (!remapValue(value, nodes))
					return false;
		const auto instancing = node.extensions.find("EXT_mesh_gpu_instancing");
		if (instancing != node.extensions.end())
		{
			if (!instancing->second.IsObject() || !instancing->second.Has("attributes") ||
				!instancing->second.Get("attributes").IsObject())
				return false;
			for (auto& [semantic, accessor] : instancing->second.Get<tinygltf::Value::Object>()["attributes"]
												  .Get<tinygltf::Value::Object>())
			{
				(void)semantic;
				if (!remapValue(accessor, accessors))
					return false;
			}
		}
	}
	for (tinygltf::Skin& skin : model.skins)
	{
		if (!remapIndex(skin.inverseBindMatrices, accessors) || !remapIndex(skin.skeleton, nodes))
			return false;
		for (int& joint : skin.joints)
			if (!remapIndex(joint, nodes, false))
				return false;
	}
	for (tinygltf::Animation& animation : model.animations)
	{
		for (tinygltf::AnimationSampler& sampler : animation.samplers)
			if (!remapIndex(sampler.input, accessors, false) || !remapIndex(sampler.output, accessors, false))
				return false;
		for (tinygltf::AnimationChannel& channel : animation.channels)
			if (!remapIndex(channel.target_node, nodes))
				return false;
	}
	for (tinygltf::Mesh& mesh : model.meshes)
		for (tinygltf::Primitive& primitive : mesh.primitives)
		{
			if (!remapIndex(primitive.indices, accessors) || !remapIndex(primitive.material, materials))
				return false;
			for (auto& [semantic, accessor] : primitive.attributes)
			{
				(void)semantic;
				if (!remapIndex(accessor, accessors, false))
					return false;
			}
			for (auto& target : primitive.targets)
				for (auto& [semantic, accessor] : target)
				{
					(void)semantic;
					if (!remapIndex(accessor, accessors, false))
						return false;
				}
			const auto variants = primitive.extensions.find("KHR_materials_variants");
			if (variants != primitive.extensions.end())
			{
				if (!variants->second.IsObject() || !variants->second.Has("mappings") ||
					!variants->second.Get("mappings").IsArray())
					return false;
				for (tinygltf::Value& mapping : variants->second.Get<tinygltf::Value::Object>()["mappings"]
													.Get<tinygltf::Value::Array>())
				{
					if (!mapping.IsObject() || !mapping.Has("material") ||
						!remapValue(mapping.Get<tinygltf::Value::Object>()["material"], materials))
						return false;
				}
			}
			const auto draco = primitive.extensions.find("KHR_draco_mesh_compression");
			if (draco != primitive.extensions.end())
			{
				if (!draco->second.IsObject() || !draco->second.Has("bufferView") ||
					!remapValue(draco->second.Get<tinygltf::Value::Object>()["bufferView"], views))
					return false;
			}
		}
	for (tinygltf::Material& material : model.materials)
	{
		for (int& lod : material.lods)
			if (!remapIndex(lod, materials, false))
				return false;
		if (!remapIndex(material.pbrMetallicRoughness.baseColorTexture.index, textures) ||
			!remapIndex(material.pbrMetallicRoughness.metallicRoughnessTexture.index, textures) ||
			!remapIndex(material.normalTexture.index, textures) ||
			!remapIndex(material.occlusionTexture.index, textures) ||
			!remapIndex(material.emissiveTexture.index, textures))
			return false;
		for (auto& [name, parameter] : material.values)
		{
			(void)name;
			const auto found = parameter.json_double_value.find("index");
			if (found != parameter.json_double_value.end() && !remapParameter(found->second, textures))
				return false;
		}
		for (auto& [name, parameter] : material.additionalValues)
		{
			(void)name;
			const auto found = parameter.json_double_value.find("index");
			if (found != parameter.json_double_value.end() && !remapParameter(found->second, textures))
				return false;
		}
		if (!remapExtensionTexture(material, "KHR_materials_anisotropy", "anisotropyTexture", textures) ||
			!remapExtensionTexture(material, "KHR_materials_clearcoat", "clearcoatTexture", textures) ||
			!remapExtensionTexture(material, "KHR_materials_clearcoat", "clearcoatRoughnessTexture",
								   textures) ||
			!remapExtensionTexture(material, "KHR_materials_clearcoat", "clearcoatNormalTexture", textures) ||
			!remapExtensionTexture(material, "KHR_materials_diffuse_transmission",
								   "diffuseTransmissionTexture", textures) ||
			!remapExtensionTexture(material, "KHR_materials_diffuse_transmission",
								   "diffuseTransmissionColorTexture", textures) ||
			!remapExtensionTexture(material, "KHR_materials_iridescence", "iridescenceTexture", textures) ||
			!remapExtensionTexture(material, "KHR_materials_iridescence", "iridescenceThicknessTexture",
								   textures) ||
			!remapExtensionTexture(material, "KHR_materials_sheen", "sheenColorTexture", textures) ||
			!remapExtensionTexture(material, "KHR_materials_sheen", "sheenRoughnessTexture", textures) ||
			!remapExtensionTexture(material, "KHR_materials_specular", "specularTexture", textures) ||
			!remapExtensionTexture(material, "KHR_materials_specular", "specularColorTexture", textures) ||
			!remapExtensionTexture(material, "KHR_materials_transmission", "transmissionTexture", textures) ||
			!remapExtensionTexture(material, "KHR_materials_volume", "thicknessTexture", textures) ||
			!remapExtensionTexture(material, "KHR_materials_pbrSpecularGlossiness", "diffuseTexture",
								   textures) ||
			!remapExtensionTexture(material, "KHR_materials_pbrSpecularGlossiness",
								   "specularGlossinessTexture", textures) ||
			!remapExtensionTexture(material, "EXT_materials_bump", "bumpTexture", textures))
			return false;
		const auto lod = material.extensions.find("MSFT_lod");
		if (lod != material.extensions.end() && lod->second.IsObject() && lod->second.Has("ids") &&
			lod->second.Get("ids").IsArray())
			for (tinygltf::Value& value :
				 lod->second.Get<tinygltf::Value::Object>()["ids"].Get<tinygltf::Value::Array>())
				if (!remapValue(value, materials))
					return false;
	}
	for (tinygltf::Texture& texture : model.textures)
	{
		if (!remapIndex(texture.sampler, samplers) || !remapIndex(texture.source, images))
			return false;
		for (const std::string_view name :
			 {std::string_view{"KHR_texture_basisu"}, std::string_view{"EXT_texture_webp"},
			  std::string_view{"EXT_texture_avif"}, std::string_view{"MSFT_texture_dds"}})
		{
			const auto extension = texture.extensions.find(std::string(name));
			if (extension == texture.extensions.end())
				continue;
			if (!extension->second.IsObject() || !extension->second.Has("source") ||
				!remapValue(extension->second.Get<tinygltf::Value::Object>()["source"], images))
				return false;
		}
	}
	for (tinygltf::Image& image : model.images)
		if (!remapIndex(image.bufferView, views))
			return false;
	for (tinygltf::Accessor& accessor : model.accessors)
	{
		if (!remapIndex(accessor.bufferView, views))
			return false;
		if (accessor.sparse.isSparse && (!remapIndex(accessor.sparse.indices.bufferView, views, false) ||
										 !remapIndex(accessor.sparse.values.bufferView, views, false)))
			return false;
	}
	for (tinygltf::BufferView& view : model.bufferViews)
	{
		if (!remapIndex(view.buffer, buffers, false))
			return false;
		const auto meshopt = view.extensions.find("EXT_meshopt_compression");
		if (meshopt != view.extensions.end())
		{
			if (!meshopt->second.IsObject() || !meshopt->second.Has("buffer") ||
				!remapValue(meshopt->second.Get<tinygltf::Value::Object>()["buffer"], buffers))
				return false;
		}
	}
	for (tinygltf::AudioEmitter& emitter : model.audioEmitters)
		if (!remapIndex(emitter.source, sources, false))
			return false;
	for (tinygltf::AudioSource& source : model.audioSources)
		if (!remapIndex(source.bufferView, views))
			return false;
	const auto rootAudio = model.extensions.find("KHR_audio");
	if (rootAudio != model.extensions.end())
	{
		if (!rootAudio->second.IsObject())
			return false;
		auto& audio = rootAudio->second.Get<tinygltf::Value::Object>();
		const auto emitterDefinitions = audio.find("emitters");
		if (emitterDefinitions != audio.end())
		{
			if (!emitterDefinitions->second.IsArray())
				return false;
			for (tinygltf::Value& emitter : emitterDefinitions->second.Get<tinygltf::Value::Array>())
			{
				if (!emitter.IsObject() || !emitter.Has("source") ||
					!remapValue(emitter.Get<tinygltf::Value::Object>()["source"], sources))
					return false;
			}
		}
		const auto sourceDefinitions = audio.find("sources");
		if (sourceDefinitions != audio.end())
		{
			if (!sourceDefinitions->second.IsArray())
				return false;
			for (tinygltf::Value& source : sourceDefinitions->second.Get<tinygltf::Value::Array>())
			{
				if (!source.IsObject())
					return false;
				if (source.Has("bufferView") &&
					!remapValue(source.Get<tinygltf::Value::Object>()["bufferView"], views))
					return false;
			}
		}
	}
	return true;
}

void filterRootExtensionArray(tinygltf::Model& model, std::string_view extension, std::string_view member,
							  const std::vector<bool>& retained)
{
	const auto found = model.extensions.find(std::string(extension));
	if (found == model.extensions.end() || !found->second.IsObject() ||
		!found->second.Has(std::string(member)) || !found->second.Get(std::string(member)).IsArray())
		return;
	auto& old =
		found->second.Get<tinygltf::Value::Object>()[std::string(member)].Get<tinygltf::Value::Array>();
	tinygltf::Value::Array filtered;
	filtered.reserve(static_cast<std::size_t>(std::count(retained.begin(), retained.end(), true)));
	const std::size_t common = std::min(old.size(), retained.size());
	for (std::size_t index = 0; index < common; ++index)
		if (retained[index])
			filtered.push_back(std::move(old[index]));
	old.swap(filtered);
}

void filterModel(tinygltf::Model& model, const Reachability& reach)
{
	filterRootExtensionArray(model, "KHR_lights_punctual", "lights", reach.lights);
	filterRootExtensionArray(model, "KHR_audio", "emitters", reach.audioEmitters);
	filterRootExtensionArray(model, "KHR_audio", "sources", reach.audioSources);
	filterVector(model.scenes, reach.scenes);
	filterVector(model.nodes, reach.nodes);
	filterVector(model.meshes, reach.meshes);
	filterVector(model.materials, reach.materials);
	filterVector(model.textures, reach.textures);
	filterVector(model.images, reach.images);
	filterVector(model.samplers, reach.samplers);
	filterVector(model.cameras, reach.cameras);
	filterVector(model.lights, reach.lights);
	filterVector(model.accessors, reach.accessors);
	filterVector(model.bufferViews, reach.bufferViews);
	filterVector(model.buffers, reach.buffers);
	filterVector(model.skins, reach.skins);
	filterVector(model.animations, reach.animations);
	filterVector(model.audioEmitters, reach.audioEmitters);
	filterVector(model.audioSources, reach.audioSources);
}

[[nodiscard]] bool dependenciesCanRemain(const GltfDocument& document,
										 const SceneCompactionResult& result) noexcept
{
	for (const GltfDependency& dependency : document.dependencies())
	{
		const CollectionResult& mapping = dependency.kind == GltfDependencyKind::buffer
											  ? collection(result, CollectionKind::buffer)
											  : collection(result, CollectionKind::image);
		const auto next = mapping.newIndex(dependency.objectIndex);
		if (!next || *next != dependency.objectIndex)
			return false;
	}
	return true;
}

detail::MutationRegistryPlan makePlan(const GltfDocument& document, const tinygltf::Model& source,
									  const SceneCompactionResult& result)
{
	detail::MutationRegistryPlan plan;
	const auto addIds = [&](EditorObjectKind kind, CollectionKind collectionKind) {
		const CollectionResult& mapping = collection(result, collectionKind);
		auto& ids = plan.idsByKind[objectKindIndex(kind)];
		ids.reserve(mapping.afterCount);
		for (std::size_t old = 0; old < mapping.oldToNew.size(); ++old)
			if (mapping.oldToNew[old])
				ids.push_back(document.idAt(kind, old));
	};
	addIds(EditorObjectKind::scene, CollectionKind::scene);
	addIds(EditorObjectKind::node, CollectionKind::node);
	addIds(EditorObjectKind::mesh, CollectionKind::mesh);
	addIds(EditorObjectKind::material, CollectionKind::material);
	addIds(EditorObjectKind::texture, CollectionKind::texture);
	addIds(EditorObjectKind::image, CollectionKind::image);
	addIds(EditorObjectKind::sampler, CollectionKind::sampler);
	addIds(EditorObjectKind::camera, CollectionKind::camera);
	addIds(EditorObjectKind::light, CollectionKind::light);
	addIds(EditorObjectKind::accessor, CollectionKind::accessor);
	addIds(EditorObjectKind::bufferView, CollectionKind::bufferView);
	addIds(EditorObjectKind::buffer, CollectionKind::buffer);
	addIds(EditorObjectKind::skin, CollectionKind::skin);
	addIds(EditorObjectKind::animation, CollectionKind::animation);

	const CollectionResult& meshMap = collection(result, CollectionKind::mesh);
	const CollectionResult& primitiveMap = collection(result, CollectionKind::primitive);
	auto& primitiveIds = plan.idsByKind[objectKindIndex(EditorObjectKind::primitive)];
	primitiveIds.reserve(primitiveMap.afterCount);
	plan.primitiveAddresses.reserve(primitiveMap.afterCount);
	std::size_t flat = 0;
	for (std::size_t mesh = 0; mesh < source.meshes.size(); ++mesh)
	{
		for (std::size_t primitive = 0; primitive < source.meshes[mesh].primitives.size(); ++primitive)
		{
			if (primitiveMap.oldToNew[flat])
			{
				primitiveIds.push_back(document.idAt(EditorObjectKind::primitive, flat));
				plan.primitiveAddresses.push_back(
					PrimitiveAddress{static_cast<std::uint32_t>(*meshMap.oldToNew[mesh]),
									 static_cast<std::uint32_t>(primitive)});
			}
			++flat;
		}
	}
	plan.currentScene = document.currentSceneId();
	plan.hierarchyChanged = false;
	return plan;
}

struct PrePublishContext final
{
	EditorFaultInjector* faultInjector{nullptr};
};

void prePublish(const detail::MutationRegistryPlan&, void* opaque)
{
	auto& context = *static_cast<PrePublishContext*>(opaque);
	if (context.faultInjector != nullptr && context.faultInjector->shouldFail(EditorFaultPoint::allocation))
		throw std::bad_alloc{};
}

[[nodiscard]] SceneCompactionStatus translateCommitStatus(detail::MutationCommitStatus status) noexcept
{
	switch (status)
	{
	case detail::MutationCommitStatus::applied:
		return SceneCompactionStatus::applied;
	case detail::MutationCommitStatus::allocationFailed:
		return SceneCompactionStatus::allocationFailed;
	case detail::MutationCommitStatus::prePublishFailed:
		return SceneCompactionStatus::injectedFailure;
	case detail::MutationCommitStatus::generationExhausted:
		return SceneCompactionStatus::generationExhausted;
	case detail::MutationCommitStatus::revisionExhausted:
		return SceneCompactionStatus::revisionExhausted;
	case detail::MutationCommitStatus::sceneActivationEpochExhausted:
	case detail::MutationCommitStatus::hierarchyEpochExhausted:
	case detail::MutationCommitStatus::externalStateSerialExhausted:
	case detail::MutationCommitStatus::internalError:
		return SceneCompactionStatus::internalError;
	}
	return SceneCompactionStatus::internalError;
}

} // namespace

SceneCompactionResult SceneCompactor::compact(GltfDocument& document, const EditorLimits& limits,
											  EditorFaultInjector* faultInjector) noexcept
{
	SceneCompactionResult result;
	result.beforeRevision = document.revision();
	result.afterRevision = result.beforeRevision;
	const tinygltf::Model& source = detail::SceneCompactorAccess::model(document);
	setBeforeCounts(result, source);
	try
	{
		if (faultInjector != nullptr && faultInjector->shouldFail(EditorFaultPoint::allocation))
		{
			result.status = SceneCompactionStatus::allocationFailed;
			setMessage(result, "compaction allocation fault was injected before candidate creation");
			return result;
		}

		const GltfValidationReport sourceValidation = GltfValidator::validate(document, limits);
		if (!sourceValidation.valid())
		{
			result.status = hasLimitIssue(sourceValidation) ? SceneCompactionStatus::limitExceeded
															: SceneCompactionStatus::invalidDocument;
			setMessage(result, firstIssueMessage(sourceValidation, "source document failed validation"));
			return result;
		}

		Reachability reach;
		ReachabilityBuilder builder(source);
		if (!builder.build(reach))
		{
			result.status = SceneCompactionStatus::invalidDocument;
			setMessage(result, "source document contains an unsupported or invalid reference");
			return result;
		}
		buildMaps(result, source, reach);
		if (!anyCollectionChanged(result))
		{
			result.status = SceneCompactionStatus::unchanged;
			setMessage(result, "document is already compact");
			return result;
		}

		std::string unsafeExtension;
		if (hasUnsafeUnknownReference(source, reach, unsafeExtension))
		{
			result.status = SceneCompactionStatus::unsafeExtensionReference;
			setMessage(result, unsafeExtension.empty()
								   ? "an opaque extension may contain an indexed reference"
								   : "an opaque extension contains an index-shaped member");
			return result;
		}
		if (!dependenciesCanRemain(document, result))
		{
			result.status = SceneCompactionStatus::dependencyRemapUnsupported;
			setMessage(result, "compaction would change externally tracked buffer or "
							   "image dependency indices");
			return result;
		}

		auto candidate = std::make_unique<tinygltf::Model>(source);
		filterModel(*candidate, reach);
		if (!rewriteReferences(*candidate, result))
		{
			result.status = SceneCompactionStatus::invalidDocument;
			setMessage(result, "a retained reference did not resolve through the compaction map");
			return result;
		}

		const GltfValidationReport candidateValidation =
			detail::SceneCompactorAccess::validateCandidate(candidate, limits);
		if (!candidateValidation.valid())
		{
			result.status = hasLimitIssue(candidateValidation) ? SceneCompactionStatus::limitExceeded
															   : SceneCompactionStatus::validationFailed;
			setMessage(result,
					   firstIssueMessage(candidateValidation, "compaction candidate failed validation"));
			return result;
		}
		if (faultInjector != nullptr && faultInjector->shouldFail(EditorFaultPoint::preCandidatePublish))
		{
			result.status = SceneCompactionStatus::injectedFailure;
			setMessage(result, "compaction publication fault was injected");
			return result;
		}

		detail::MutationRegistryPlan plan = makePlan(document, source, result);
		PrePublishContext context{faultInjector};
		const detail::MutationCommitStatus commitStatus =
			detail::SceneCompactorAccess::commit(document, std::move(candidate), plan, prePublish, &context);
		result.status = translateCommitStatus(commitStatus);
		if (result.status != SceneCompactionStatus::applied)
		{
			setMessage(result, result.status == SceneCompactionStatus::allocationFailed
								   ? "compaction candidate publication could not allocate"
								   : "compaction candidate could not be published atomically");
			return result;
		}
		result.afterRevision = document.revision();
		setMessage(result, "unreachable glTF objects removed");
		return result;
	}
	catch (const std::bad_alloc&)
	{
		result.status = SceneCompactionStatus::allocationFailed;
		result.afterRevision = document.revision();
		setMessage(result, "compaction allocation failed before publication");
		return result;
	}
	catch (...)
	{
		result.status = SceneCompactionStatus::internalError;
		result.afterRevision = document.revision();
		setMessage(result, "compaction failed before atomic publication");
		return result;
	}
}

} // namespace demo::editor
