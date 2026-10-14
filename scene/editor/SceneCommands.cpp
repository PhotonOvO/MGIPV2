#include <mgif/scene_editor/SceneCommands.h>
#include <mgif/scene_editor/SceneChangeCanonicalizer.h>
#include <mgif/scene_editor/detail/BitwiseCast.h>

#include <tiny_gltf.h>


#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace demo::editor {
namespace detail {
class SceneCommandsAccess final
{
public:
	[[nodiscard]] static const tinygltf::Model& model(const GltfDocument& document) noexcept
	{
		return document.authorityModel();
	}
	[[nodiscard]] static bool commit(GltfDocument& document,
		std::unique_ptr<tinygltf::Model> model,
		MutationRegistryPlan& plan,
		MutationPrePublishCallback prePublish,
		void* prePublishContext,
		MutationCommitStatus& status) noexcept
	{
		status = document.commitMutation(
			std::move(model), plan, prePublish, prePublishContext);
		return status == MutationCommitStatus::applied;
	}
	[[nodiscard]] static MutationCommitStatus commitNodeName(
		GltfDocument& document, std::size_t nodeIndex, std::string name,
		MutationRegistryPlan& plan, MutationPrePublishCallback prePublish,
		void* prePublishContext) noexcept
	{
		return document.commitNodeNameMutation(nodeIndex, std::move(name), plan,
			prePublish, prePublishContext);
	}
	[[nodiscard]] static MutationCommitStatus commitMaterialProperties(
		GltfDocument& document, std::size_t materialIndex,
		const tinygltf::Material& record, MutationRegistryPlan& plan,
		MutationPrePublishCallback prePublish, void* prePublishContext) noexcept
	{
		return document.commitMaterialPropertiesMutation(materialIndex, record, plan,
			prePublish, prePublishContext);
	}
	[[nodiscard]] static MutationCommitStatus resolvePreparedIds(
		const GltfDocument& document, MutationRegistryPlan& plan) noexcept
	{
		return document.resolvePreparedMutationIds(plan);
	}
	[[nodiscard]] static GltfValidationReport validateModel(
		const tinygltf::Model& model, const EditorLimits& limits)
	{
		return GltfValidator::validateModel(model, limits);
	}
	[[nodiscard]] static SceneDigest documentDigest(const tinygltf::Model& model)
	{
		return GltfDocument::documentDigest(model);
	}
};
} // namespace detail

namespace {

using Plan = detail::MutationRegistryPlan;

[[nodiscard]] detail::MutationSceneRootStatePlan rootStatePlan(
	SceneId scene, const SceneRootTransformState& state,
	std::vector<NodeId> frozenRoots, bool preserveExternal)
{
	detail::MutationSceneRootStatePlan result;
	result.scene = scene.untyped();
	result.baselineTranslation = state.baseline.translation;
	result.baselineRotation = state.baseline.rotation;
	result.baselineScale = state.baseline.scale;
	result.currentTranslation = state.current.translation;
	result.currentRotation = state.current.rotation;
	result.currentScale = state.current.scale;
	result.expectedExternalStateSerial = state.externalStateSerial;
	result.targetExternalStateSerial = state.externalStateSerial;
	result.frozen = state.frozen;
	result.preserveExternalOnMismatch = preserveExternal;
	result.frozenRootIds.reserve(frozenRoots.size());
	for(const NodeId root : frozenRoots) result.frozenRootIds.push_back(root.untyped());
	return result;
}

constinit std::atomic<std::uint64_t> g_nextMutationSerial{1u};

[[nodiscard]] std::uint64_t allocateMutationSerial() noexcept
{
	std::uint64_t candidate = g_nextMutationSerial.load(std::memory_order_relaxed);
	while(candidate != 0)
	{
		const std::uint64_t successor = candidate == std::numeric_limits<std::uint64_t>::max()
			? 0u : candidate + 1u;
		if(g_nextMutationSerial.compare_exchange_weak(candidate, successor,
			std::memory_order_relaxed, std::memory_order_relaxed))
		{
			return candidate;
		}
	}
	return 0;
}

[[nodiscard]] std::uint64_t nextRevisionWithoutWrap(std::uint64_t revision) noexcept
{
	return revision == std::numeric_limits<std::uint64_t>::max()
		? revision : revision + 1u;
}

constexpr std::size_t kindIndex(EditorObjectKind kind) noexcept
{
	return static_cast<std::size_t>(kind);
}

bool finite(const std::array<double, 3>& values) noexcept
{
	return std::all_of(values.begin(), values.end(), [](double value) { return std::isfinite(value); });
}

bool finite(const std::array<double, 4>& values) noexcept
{
	return std::all_of(values.begin(), values.end(), [](double value) { return std::isfinite(value); });
}

bool finite(double value) noexcept
{
	return std::isfinite(value);
}

bool finiteSceneRoot(const SceneRootTransform& transform) noexcept
{
	for(const double value : transform.translation) if(!std::isfinite(value)) return false;
	for(const double value : transform.rotation) if(!std::isfinite(value)) return false;
	for(const double value : transform.scale) if(!std::isfinite(value)) return false;
	const double norm = std::sqrt(std::inner_product(transform.rotation.begin(), transform.rotation.end(),
		transform.rotation.begin(), 0.0));
	return std::isfinite(norm) && std::abs(norm - 1.0) <= 1.0e-4
		&& std::abs(transform.scale[0]) > 1.0e-12
		&& std::abs(transform.scale[1]) > 1.0e-12
		&& std::abs(transform.scale[2]) > 1.0e-12;
}

using Matrix4 = std::array<double, 16>;

Matrix4 multiplyMatrix(const Matrix4& left, const Matrix4& right)
{
	Matrix4 result{};
	for(std::size_t column = 0; column < 4; ++column)
		for(std::size_t row = 0; row < 4; ++row)
			for(std::size_t inner = 0; inner < 4; ++inner)
				result[column * 4u + row] += left[inner * 4u + row] * right[column * 4u + inner];
	return result;
}

Matrix4 sceneRootMatrix(const SceneRootTransform& transform)
{
	const double x = transform.rotation[0], y = transform.rotation[1], z = transform.rotation[2], w = transform.rotation[3];
	const double sx = transform.scale[0], sy = transform.scale[1], sz = transform.scale[2];
	return Matrix4{
		(1.0 - 2.0 * (y * y + z * z)) * sx, (2.0 * (x * y + z * w)) * sx, (2.0 * (x * z - y * w)) * sx, 0.0,
		(2.0 * (x * y - z * w)) * sy, (1.0 - 2.0 * (x * x + z * z)) * sy, (2.0 * (y * z + x * w)) * sy, 0.0,
		(2.0 * (x * z + y * w)) * sz, (2.0 * (y * z - x * w)) * sz, (1.0 - 2.0 * (x * x + y * y)) * sz, 0.0,
		transform.translation[0], transform.translation[1], transform.translation[2], 1.0};
}

[[nodiscard]] bool identitySceneRoot(const SceneRootTransform& transform) noexcept
{
	return transform.translation == SceneRootTransform{}.translation
		&& transform.rotation == SceneRootTransform{}.rotation
		&& transform.scale == SceneRootTransform{}.scale;
}

[[nodiscard]] bool sameSceneRoot(
	const SceneRootTransform& left, const SceneRootTransform& right) noexcept
{
	return left.translation == right.translation
		&& left.rotation == right.rotation
		&& left.scale == right.scale;
}

void bakeNodeTransform(tinygltf::Node& node, const SceneRootTransform& transform)

{
	const Matrix4 rootTransform = sceneRootMatrix(transform);
	Matrix4 local{1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
		0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
	if(node.matrix.size() == 16)
	{
		for(std::size_t value = 0; value < 16; ++value) local[value] = node.matrix[value];
	}
	else
	{
		const double tx = node.translation.size() == 3 ? node.translation[0] : 0.0;
		const double ty = node.translation.size() == 3 ? node.translation[1] : 0.0;
		const double tz = node.translation.size() == 3 ? node.translation[2] : 0.0;
		const double x = node.rotation.size() == 4 ? node.rotation[0] : 0.0;
		const double y = node.rotation.size() == 4 ? node.rotation[1] : 0.0;
		const double z = node.rotation.size() == 4 ? node.rotation[2] : 0.0;
		const double w = node.rotation.size() == 4 ? node.rotation[3] : 1.0;
		const double sx = node.scale.size() == 3 ? node.scale[0] : 1.0;
		const double sy = node.scale.size() == 3 ? node.scale[1] : 1.0;
		const double sz = node.scale.size() == 3 ? node.scale[2] : 1.0;
		local = Matrix4{
			(1.0 - 2.0 * (y * y + z * z)) * sx, (2.0 * (x * y + z * w)) * sx, (2.0 * (x * z - y * w)) * sx, 0.0,
			(2.0 * (x * y - z * w)) * sy, (1.0 - 2.0 * (x * x + z * z)) * sy, (2.0 * (y * z + x * w)) * sy, 0.0,
			(2.0 * (x * z + y * w)) * sz, (2.0 * (y * z - x * w)) * sz, (1.0 - 2.0 * (x * x + y * y)) * sz, 0.0,
			tx, ty, tz, 1.0};
	}
	const Matrix4 baked = multiplyMatrix(rootTransform, local);
	node.matrix.assign(baked.begin(), baked.end());
	node.translation.clear();
	node.rotation.clear();
	node.scale.clear();
}

[[nodiscard]] MutationErrorCode bakeSceneRootPreviews(
	const GltfDocument& document, tinygltf::Model& model, Plan& plan)
{
	struct RootUse final
	{
		SceneRootTransform transform{};
		bool initialized{false};
		bool bake{false};
		bool alreadyFrozen{false};
		std::vector<std::size_t> scenes;
	};

	plan.frozenSceneRootIndicesByScene.assign(model.scenes.size(), {});
	std::unordered_map<std::size_t, RootUse> rootUses;
	for(std::size_t sceneIndex = 0; sceneIndex < model.scenes.size(); ++sceneIndex)
	{
		SceneId scene;
		if(sceneIndex < plan.idsByKind[kindIndex(EditorObjectKind::scene)].size())
		{
			scene = SceneId::from(
				plan.idsByKind[kindIndex(EditorObjectKind::scene)][sceneIndex]).value_or(SceneId{});
		}
		const SceneRootTransform transform = scene.valid()
			? document.sceneRootTransform(scene) : SceneRootTransform{};
		for(const int root : model.scenes[sceneIndex].nodes)
		{
			if(root < 0 || static_cast<std::size_t>(root) >= model.nodes.size())
			{
				return MutationErrorCode::invalidReference;
			}
			const std::size_t rootIndex = static_cast<std::size_t>(root);
			NodeId rootId;
			if(rootIndex < plan.idsByKind[kindIndex(EditorObjectKind::node)].size())
			{
				rootId = NodeId::from(
					plan.idsByKind[kindIndex(EditorObjectKind::node)][rootIndex]).value_or(NodeId{});
			}
			const bool alreadyFrozen = scene.valid() && rootId.valid()
				&& document.sceneRootNodeFrozen(scene, rootId);
			const SceneRootTransform desired = alreadyFrozen ? SceneRootTransform{} : transform;
			auto& use = rootUses[rootIndex];
			if(use.initialized && (use.alreadyFrozen != alreadyFrozen
				|| !sameSceneRoot(use.transform, desired)))
			{
				return MutationErrorCode::multiScenePolicy;
			}
			if(!use.initialized)
			{
				use.transform = desired;
				use.initialized = true;
				use.bake = !identitySceneRoot(desired);
				use.alreadyFrozen = alreadyFrozen;
			}
			use.scenes.push_back(sceneIndex);
			if(alreadyFrozen)
			{
				plan.frozenSceneRootIndicesByScene[sceneIndex].push_back(rootIndex);
			}
		}
	}

	for(auto& [rootIndex, use] : rootUses)
	{
		if(!use.bake) continue;
		bakeNodeTransform(model.nodes[rootIndex], use.transform);
		for(const std::size_t sceneIndex : use.scenes)
		{
			auto& frozen = plan.frozenSceneRootIndicesByScene[sceneIndex];
			if(std::find(frozen.begin(), frozen.end(), rootIndex) == frozen.end())
			{
				frozen.push_back(rootIndex);
			}
		}
	}
	return MutationErrorCode::none;
}

bool sameTransform(const tinygltf::Node& node, const NodeTransform& transform) noexcept
{
	return node.matrix.empty()
		&& node.translation == std::vector<double>(transform.translation.begin(), transform.translation.end())
		&& node.rotation == std::vector<double>(transform.rotation.begin(), transform.rotation.end())
		&& node.scale == std::vector<double>(transform.scale.begin(), transform.scale.end());
}

template <typename T>
void remapIndex(T& value, const std::vector<int>& map)
{
	if(value < 0) return;
	const std::size_t old = static_cast<std::size_t>(value);
	value = old < map.size() ? map[old] : -1;
}

void remapIndices(std::vector<int>& values, const std::vector<int>& map, bool dropMissing = true)
{
	std::vector<int> remapped;
	remapped.reserve(values.size());
	for(int value : values)
	{
		if(value < 0 || static_cast<std::size_t>(value) >= map.size())
		{
			if(!dropMissing) remapped.push_back(-1);
			continue;
		}
		const int mapped = map[static_cast<std::size_t>(value)];
		if(mapped >= 0 || !dropMissing) remapped.push_back(mapped);
	}
	values.swap(remapped);
}

void ensureExtensionUsed(tinygltf::Model& model, const std::string& name)
{
	if(std::find(model.extensionsUsed.begin(), model.extensionsUsed.end(), name)
		== model.extensionsUsed.end())
	{
		model.extensionsUsed.push_back(name);
	}
}

void removeNodeExtensionUsedIfUnreferenced(tinygltf::Model& model, const std::string& name)
{
	const bool stillUsed = std::any_of(model.nodes.begin(), model.nodes.end(),
		[&](const tinygltf::Node& node) { return node.extensions.contains(name); });
	if(!stillUsed)
	{
		model.extensionsUsed.erase(
			std::remove(model.extensionsUsed.begin(), model.extensionsUsed.end(), name),
			model.extensionsUsed.end());
		model.extensionsRequired.erase(
			std::remove(model.extensionsRequired.begin(), model.extensionsRequired.end(), name),
			model.extensionsRequired.end());
	}
}

void removeMaterialExtensionUsedIfUnreferenced(
	tinygltf::Model& model, const std::string& name)
{
	const auto collectionHas = [&](const auto& collection)
	{
		return std::any_of(collection.begin(), collection.end(),
			[&](const auto& item) { return item.extensions.contains(name); });
	};
	if(model.extensions.contains(name) || model.asset.extensions.contains(name)
		|| collectionHas(model.accessors) || collectionHas(model.animations)
		|| collectionHas(model.buffers) || collectionHas(model.bufferViews)
		|| collectionHas(model.cameras) || collectionHas(model.images)
		|| collectionHas(model.lights) || collectionHas(model.materials)
		|| collectionHas(model.meshes) || collectionHas(model.nodes)
		|| collectionHas(model.samplers) || collectionHas(model.scenes)
		|| collectionHas(model.skins) || collectionHas(model.textures)) return;
	for(const auto& mesh : model.meshes)
		if(collectionHas(mesh.primitives)) return;
	for(const auto& animation : model.animations)
		if(collectionHas(animation.channels) || collectionHas(animation.samplers)) return;
	for(const auto& material : model.materials)
	{
		if(material.pbrMetallicRoughness.extensions.contains(name)
			|| material.pbrMetallicRoughness.baseColorTexture.extensions.contains(name)
			|| material.pbrMetallicRoughness.metallicRoughnessTexture.extensions.contains(name)
			|| material.normalTexture.extensions.contains(name)
			|| material.occlusionTexture.extensions.contains(name)
			|| material.emissiveTexture.extensions.contains(name)) return;
	}
	model.extensionsUsed.erase(
		std::remove(model.extensionsUsed.begin(), model.extensionsUsed.end(), name),
		model.extensionsUsed.end());
	model.extensionsRequired.erase(
		std::remove(model.extensionsRequired.begin(), model.extensionsRequired.end(), name),
		model.extensionsRequired.end());
}

[[nodiscard]] bool materialPropertiesEqual(
	const tinygltf::Material& material, const MaterialProperties& properties) noexcept
{
	return material.pbrMetallicRoughness.baseColorFactor.size() == properties.baseColorFactor.size()
		&& std::equal(material.pbrMetallicRoughness.baseColorFactor.begin(),
			material.pbrMetallicRoughness.baseColorFactor.end(), properties.baseColorFactor.begin())
		&& material.pbrMetallicRoughness.metallicFactor == properties.metallicFactor
		&& material.pbrMetallicRoughness.roughnessFactor == properties.roughnessFactor
		&& material.emissiveFactor.size() == properties.emissiveFactor.size()
		&& std::equal(material.emissiveFactor.begin(), material.emissiveFactor.end(),
			properties.emissiveFactor.begin())
		&& material.alphaCutoff == properties.alphaCutoff
		&& material.alphaMode == properties.alphaMode
		&& material.doubleSided == properties.doubleSided;
}

[[nodiscard]] bool validMaterialProperties(const MaterialProperties& properties) noexcept
{
	if(!finite(properties.baseColorFactor) || !finite(properties.emissiveFactor)
		|| !finite(properties.metallicFactor) || !finite(properties.roughnessFactor)
		|| !finite(properties.alphaCutoff)) return false;
	if(properties.metallicFactor < 0.0 || properties.metallicFactor > 1.0
		|| properties.roughnessFactor < 0.0 || properties.roughnessFactor > 1.0)
		return false;
	if(properties.alphaCutoff < 0.0) return false;
	if(std::any_of(properties.baseColorFactor.begin(), properties.baseColorFactor.end(),
		[](double value) { return value < 0.0 || value > 1.0; })) return false;
	if(std::any_of(properties.emissiveFactor.begin(), properties.emissiveFactor.end(),
		[](double value) { return value < 0.0; })) return false;
	return properties.alphaMode == "OPAQUE" || properties.alphaMode == "MASK"
		|| properties.alphaMode == "BLEND";
}

[[nodiscard]] bool extensionValueToTiny(
	const MaterialExtensionValue& source,
	tinygltf::Value& destination,
	const EditorLimits& limits,
	std::size_t depth,
	std::size_t& valueCount)
{
	if(depth > limits.maxJsonDepth || valueCount >= limits.maxJsonValues) return false;
	++valueCount;
	if(std::holds_alternative<std::monostate>(source.value))
	{
		destination = tinygltf::Value{};
		return true;
	}
	if(const auto* value = std::get_if<bool>(&source.value))
	{
		destination = tinygltf::Value(*value);
		return true;
	}
	if(const auto* value = std::get_if<std::int32_t>(&source.value))
	{
		destination = tinygltf::Value(static_cast<int>(*value));
		return true;
	}
	if(const auto* value = std::get_if<double>(&source.value))
	{
		if(!std::isfinite(*value)) return false;
		destination = tinygltf::Value(*value);
		return true;
	}
	if(const auto* value = std::get_if<std::string>(&source.value))
	{
		if(value->size() > limits.maxStringBytes) return false;
		destination = tinygltf::Value(*value);
		return true;
	}
	if(const auto* array = std::get_if<MaterialExtensionValue::Array>(&source.value))
	{
		tinygltf::Value::Array converted;
		converted.reserve(array->size());
		for(const auto& item : *array)
		{
			tinygltf::Value child;
			if(!extensionValueToTiny(item, child, limits, depth + 1u, valueCount)) return false;
			converted.push_back(std::move(child));
		}
		destination = tinygltf::Value(std::move(converted));
		return true;
	}
	const auto* object = std::get_if<MaterialExtensionValue::Object>(&source.value);
	if(object == nullptr) return false;
	tinygltf::Value::Object converted;
	for(const auto& [name, item] : *object)
	{
		if(name.empty() || name.size() > limits.maxStringBytes || converted.contains(name)) return false;
		tinygltf::Value child;
		if(!extensionValueToTiny(item, child, limits, depth + 1u, valueCount)) return false;
		converted.emplace(name, std::move(child));
	}
	destination = tinygltf::Value(std::move(converted));
	return true;
}

[[nodiscard]] bool extensionValueFromTiny(
	const tinygltf::Value& source,
	MaterialExtensionValue& destination,
	const EditorLimits& limits,
	std::size_t depth,
	std::size_t& valueCount)
{
	if(depth > limits.maxJsonDepth || valueCount >= limits.maxJsonValues) return false;
	++valueCount;
	if(source.Type() == tinygltf::NULL_TYPE)
	{
		destination.value = std::monostate{};
		return true;
	}
	if(source.IsBool()) { destination.value = source.Get<bool>(); return true; }
	if(source.IsInt()) { destination.value = static_cast<std::int32_t>(source.Get<int>()); return true; }
	if(source.IsReal())
	{
		const double value = source.Get<double>();
		if(!std::isfinite(value)) return false;
		destination.value = value;
		return true;
	}
	if(source.IsString())
	{
		const auto& value = source.Get<std::string>();
		if(value.size() > limits.maxStringBytes) return false;
		destination.value = value;
		return true;
	}
	if(source.IsArray())
	{
		MaterialExtensionValue::Array converted;
		const auto& array = source.Get<tinygltf::Value::Array>();
		converted.reserve(array.size());
		for(const auto& item : array)
		{
			MaterialExtensionValue child;
			if(!extensionValueFromTiny(item, child, limits, depth + 1u, valueCount)) return false;
			converted.push_back(std::move(child));
		}
		destination.value = std::move(converted);
		return true;
	}
	if(!source.IsObject()) return false;
	MaterialExtensionValue::Object converted;
	const auto& object = source.Get<tinygltf::Value::Object>();
	converted.reserve(object.size());
	for(const auto& [name, item] : object)
	{
		MaterialExtensionValue child;
		if(!extensionValueFromTiny(item, child, limits, depth + 1u, valueCount)) return false;
		converted.emplace_back(name, std::move(child));
	}
	destination.value = std::move(converted);
	return true;
}

[[nodiscard]] bool validLightProperties(const LightProperties& properties) noexcept
{
	constexpr double halfPi = 1.57079632679489661923;
	if(properties.type != "point" && properties.type != "spot"
		&& properties.type != "directional") return false;
	if(!finite(properties.color) || !finite(properties.intensity) || !finite(properties.range)
		|| !finite(properties.innerConeAngle) || !finite(properties.outerConeAngle)) return false;
	if(properties.intensity < 0.0 || properties.range < 0.0
		|| std::any_of(properties.color.begin(), properties.color.end(),
			[](double value) { return value < 0.0; })) return false;
	return properties.type != "spot" || (properties.innerConeAngle >= 0.0
			&& properties.innerConeAngle < properties.outerConeAngle
			&& properties.outerConeAngle <= halfPi);
}

[[nodiscard]] LightProperties lightPropertiesOf(const tinygltf::Light& light)
{
	LightProperties result;
	result.type = light.type;
	if(light.color.size() == 3)
		std::copy_n(light.color.begin(), 3, result.color.begin());
	result.intensity = light.intensity;
	result.range = light.range;
	if(light.type == "spot")
	{
		result.innerConeAngle = light.spot.innerConeAngle;
		result.outerConeAngle = light.spot.outerConeAngle;
	}
	return result;
}

void applyLightProperties(tinygltf::Light& light, const LightProperties& properties)
{
	light.type = properties.type;
	light.color.assign(properties.color.begin(), properties.color.end());
	light.intensity = properties.intensity;
	light.range = properties.range;
	light.spot.innerConeAngle = properties.type == "spot"
		? properties.innerConeAngle : LightProperties{}.innerConeAngle;
	light.spot.outerConeAngle = properties.type == "spot"
		? properties.outerConeAngle : LightProperties{}.outerConeAngle;
}

Plan makePlan(const GltfDocument& document)
{
	Plan plan;
	for(std::size_t kind = 1; kind < kindIndex(EditorObjectKind::count); ++kind)
	{
		const auto typedKind = static_cast<EditorObjectKind>(kind);
		plan.idsByKind[kind].reserve(document.objectCount(typedKind));
		for(std::size_t index = 0; index < document.objectCount(typedKind); ++index)
		{
			plan.idsByKind[kind].push_back(document.idAt(typedKind, index));
		}
	}
	for(std::size_t index = 0; index < document.objectCount(EditorObjectKind::primitive); ++index)
	{
		if(const auto address = document.primitiveAddress(
			PrimitiveId::from(document.idAt(EditorObjectKind::primitive, index)).value()))
		{
			plan.primitiveAddresses.push_back(*address);
		}
	}
	plan.currentScene = document.currentSceneId();
	return plan;
}

std::vector<MutationRemapEntry>& remapFor(MutationRemap& remap, EditorObjectKind kind)
{
	switch(kind)
	{
		case EditorObjectKind::scene: return remap.scenes;
		case EditorObjectKind::node: return remap.nodes;
		case EditorObjectKind::primitive: return remap.primitives;
		case EditorObjectKind::mesh: return remap.meshes;
		case EditorObjectKind::material: return remap.materials;
		case EditorObjectKind::texture: return remap.textures;
		case EditorObjectKind::image: return remap.images;
		case EditorObjectKind::sampler: return remap.samplers;
		case EditorObjectKind::accessor: return remap.accessors;
		case EditorObjectKind::bufferView: return remap.bufferViews;
		case EditorObjectKind::buffer: return remap.buffers;
		case EditorObjectKind::skin: return remap.skins;
		case EditorObjectKind::animation: return remap.animations;
		case EditorObjectKind::camera: return remap.cameras;
		case EditorObjectKind::light: return remap.lights;
		default: return remap.nodes;
	}
}

void addRemap(MutationResult& result,
	           const std::vector<EditorObjectId>& before,
	           const std::vector<EditorObjectId>& after,
	           EditorObjectKind kind)
{
	auto& entries = remapFor(result.remap, kind);
	std::unordered_set<EditorObjectId, EditorObjectIdHash> live(after.begin(), after.end());
	for(const EditorObjectId id : before)
	{
		entries.push_back(MutationRemapEntry{id, live.contains(id) ? id : EditorObjectId{}});
	}
	for(const EditorObjectId id : after)
	{
		if(std::find(before.begin(), before.end(), id) == before.end()) entries.push_back(MutationRemapEntry{{}, id});
	}
}

[[nodiscard]] auto stableKey(EditorObjectId id) noexcept
{
	return std::tuple{static_cast<std::uint8_t>(id.kind), id.authority.value,
		id.slot, id.generation};
}

[[nodiscard]] bool sameStableSlot(EditorObjectId left, EditorObjectId right) noexcept
{
	return left.kind == right.kind && left.authority == right.authority
		&& left.slot == right.slot;
}

void addExactRemap(SceneMutationEvidence& evidence,
	const std::vector<EditorObjectId>& before,
	const std::vector<EditorObjectId>& after)
{
	std::vector<bool> consumed(after.size(), false);
	for(const EditorObjectId oldId : before)
	{
		if(std::find(after.begin(), after.end(), oldId) != after.end()) continue;
		std::optional<std::size_t> replacement;
		for(std::size_t index = 0; index < after.size(); ++index)
		{
			if(!consumed[index] && sameStableSlot(oldId, after[index]))
			{
				replacement = index;
				break;
			}
		}
		if(replacement)
		{
			consumed[*replacement] = true;
			evidence.stableIdRemap.push_back(StableIdRemapEvidence{
				stableSceneId(oldId), stableSceneId(after[*replacement])});
		}
		else
		{
			evidence.stableIdRemap.push_back(StableIdRemapEvidence{
				stableSceneId(oldId), std::nullopt});
		}
	}
	for(std::size_t index = 0; index < after.size(); ++index)
	{
		const EditorObjectId newId = after[index];
		if(consumed[index]
			|| std::find(before.begin(), before.end(), newId) != before.end()) continue;
		evidence.stableIdRemap.push_back(StableIdRemapEvidence{
			std::nullopt, stableSceneId(newId)});
	}
}

[[nodiscard]] SceneMutationToken sceneMutationToken(MutationToken token) noexcept
{
	return SceneMutationToken{token.authority, token.revision, token.serial};
}

[[nodiscard]] SceneNodeTransformValue nodeTransformValue(const tinygltf::Node& node)
{
	if(node.matrix.size() == 16u)
	{
		SceneMatrixValue matrix;
		std::copy_n(node.matrix.begin(), matrix.columnMajor.size(),
			matrix.columnMajor.begin());
		return SceneNodeTransformValue{std::move(matrix)};
	}
	SceneTrsValue trs;
	if(node.translation.size() == trs.translation.size())
		std::copy(node.translation.begin(), node.translation.end(), trs.translation.begin());
	if(node.rotation.size() == trs.rotation.size())
		std::copy(node.rotation.begin(), node.rotation.end(), trs.rotation.begin());
	if(node.scale.size() == trs.scale.size())
		std::copy(node.scale.begin(), node.scale.end(), trs.scale.begin());
	return SceneNodeTransformValue{std::move(trs)};
}

[[nodiscard]] bool nodeLocalVisibility(const tinygltf::Node& node) noexcept
{
	const auto extension = node.extensions.find("KHR_node_visibility");
	return extension == node.extensions.end() || !extension->second.IsObject()
		|| !extension->second.Has("visible")
		|| !extension->second.Get("visible").IsBool()
		|| extension->second.Get("visible").Get<bool>();
}

[[nodiscard]] SceneAlphaMode alphaMode(std::string_view value) noexcept
{
	if(value == "MASK") return SceneAlphaMode::Mask;
	if(value == "BLEND") return SceneAlphaMode::Blend;
	return SceneAlphaMode::Opaque;
}

[[nodiscard]] SceneMaterialParametersValue materialParameters(
	const tinygltf::Material& material)
{
	SceneMaterialParametersValue value;
	if(material.pbrMetallicRoughness.baseColorFactor.size()
		== value.baseColorFactor.size())
		std::copy(material.pbrMetallicRoughness.baseColorFactor.begin(),
			material.pbrMetallicRoughness.baseColorFactor.end(),
			value.baseColorFactor.begin());
	value.metallicFactor = material.pbrMetallicRoughness.metallicFactor;
	value.roughnessFactor = material.pbrMetallicRoughness.roughnessFactor;
	if(material.emissiveFactor.size() == value.emissiveFactor.size())
		std::copy(material.emissiveFactor.begin(), material.emissiveFactor.end(),
			value.emissiveFactor.begin());
	value.alphaCutoff = material.alphaCutoff;
	value.alphaMode = alphaMode(material.alphaMode);
	value.doubleSided = material.doubleSided;
	return value;
}

void appendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
	for(unsigned int shift = 0; shift < 64u; shift += 8u)
		bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
}

void appendTinyValue(std::vector<std::uint8_t>& bytes, const tinygltf::Value& value)
{
	if(!value.IsBool() && !value.IsInt() && !value.IsNumber()
		&& !value.IsString() && !value.IsBinary() && !value.IsArray()
		&& !value.IsObject())
	{
		bytes.push_back(0u);
	}
	else if(value.IsBool())
	{
		bytes.push_back(1u);
		bytes.push_back(value.Get<bool>() ? 1u : 0u);
	}
	else if(value.IsInt())
	{
		bytes.push_back(2u);
		appendU64(bytes, static_cast<std::uint64_t>(
			static_cast<std::int64_t>(value.Get<int>())));
	}
	else if(value.IsNumber())
	{
		bytes.push_back(3u);
		appendU64(bytes, detail::bitwiseCast<std::uint64_t>(value.Get<double>()));
	}
	else if(value.IsString())
	{
		bytes.push_back(4u);
		const std::string& string = value.Get<std::string>();
		appendU64(bytes, string.size());
		bytes.insert(bytes.end(), string.begin(), string.end());
	}
	else if(value.IsBinary())
	{
		bytes.push_back(5u);
		const auto& binary = value.Get<std::vector<unsigned char>>();
		appendU64(bytes, binary.size());
		bytes.insert(bytes.end(), binary.begin(), binary.end());
	}
	else if(value.IsArray())
	{
		bytes.push_back(6u);
		const auto& array = value.Get<tinygltf::Value::Array>();
		appendU64(bytes, array.size());
		for(const tinygltf::Value& child : array) appendTinyValue(bytes, child);
	}
	else if(value.IsObject())
	{
		bytes.push_back(7u);
		const auto& object = value.Get<tinygltf::Value::Object>();
		appendU64(bytes, object.size());
		for(const auto& [name, child] : object)
		{
			appendU64(bytes, name.size());
			bytes.insert(bytes.end(), name.begin(), name.end());
			appendTinyValue(bytes, child);
		}
	}
	else
	{
		bytes.push_back(0u);
	}
}

[[nodiscard]] SceneMaterialExtensionValue materialExtension(
	std::string name, const tinygltf::Value& source)
{
	SceneMaterialExtensionValue value;
	value.extensionName = std::move(name);
	appendTinyValue(value.canonicalValueBytes, source);
	value.contentDigest = SceneChangeCanonicalizer::computeSha256(
		value.canonicalValueBytes);
	return value;
}

[[nodiscard]] std::optional<StableSceneId> stableIdAt(
	const std::vector<EditorObjectId>& ids, int index) noexcept
{
	if(index < 0 || static_cast<std::size_t>(index) >= ids.size()) return std::nullopt;
	const EditorObjectId id = ids[static_cast<std::size_t>(index)];
	return id.valid() ? std::optional<StableSceneId>{stableSceneId(id)} : std::nullopt;
}

[[nodiscard]] SceneTextureBindingValue textureBinding(
	SceneTextureSemantic semantic, int textureIndex, int uvSet, double scalar,
	const tinygltf::Model& model, const Plan& plan)
{
	SceneTextureBindingValue value;
	value.semantic = semantic;
	value.uvSet = uvSet >= 0 ? static_cast<std::uint32_t>(uvSet) : 0u;
	value.scalar = scalar;
	value.texture = stableIdAt(
		plan.idsByKind[kindIndex(EditorObjectKind::texture)], textureIndex);
	if(textureIndex >= 0 && static_cast<std::size_t>(textureIndex) < model.textures.size())
	{
		value.sampler = stableIdAt(
			plan.idsByKind[kindIndex(EditorObjectKind::sampler)],
			model.textures[static_cast<std::size_t>(textureIndex)].sampler);
	}
	return value;
}

[[nodiscard]] SceneCameraValue cameraValue(const tinygltf::Camera& camera)
{
	SceneCameraValue value;
	value.projection = camera.type == "orthographic"
		? SceneCameraProjection::Orthographic : SceneCameraProjection::Perspective;
	value.yfov = camera.perspective.yfov;
	value.znear = camera.type == "orthographic"
		? camera.orthographic.znear : camera.perspective.znear;
	value.zfar = camera.type == "orthographic"
		? camera.orthographic.zfar : camera.perspective.zfar;
	value.aspectRatio = camera.perspective.aspectRatio;
	value.xmag = camera.orthographic.xmag;
	value.ymag = camera.orthographic.ymag;
	return value;
}

[[nodiscard]] SceneLightType lightType(std::string_view value) noexcept
{
	if(value == "directional") return SceneLightType::Directional;
	if(value == "spot") return SceneLightType::Spot;
	return SceneLightType::Point;
}

[[nodiscard]] SceneLightValue lightValue(const tinygltf::Light& light)
{
	SceneLightValue value;
	value.type = lightType(light.type);
	if(light.color.size() == value.color.size())
		std::copy(light.color.begin(), light.color.end(), value.color.begin());
	value.intensity = light.intensity;
	value.range = light.range;
	value.innerConeAngle = light.spot.innerConeAngle;
	value.outerConeAngle = light.spot.outerConeAngle;
	return value;
}

template<typename Value>
void addUpdate(SceneMutationEvidence& evidence, StableSceneId target,
	SceneChangeClass changeClass, SceneField field, std::uint64_t serial,
	Value before, Value after)
{
	if(before == after) return;
	SceneFieldChange change;
	change.target = target;
	change.changeClass = changeClass;
	change.action = SceneChangeAction::Update;
	change.field = field;
	change.mutationSerial = serial;
	change.beforeValue = SceneFieldValue{std::move(before)};
	change.afterValue = SceneFieldValue{std::move(after)};
	evidence.exactChanges.push_back(std::move(change));
}

[[nodiscard]] std::optional<StableSceneId> attachedNode(
	const tinygltf::Model& model, const Plan& plan, EditorObjectKind kind,
	std::size_t objectIndex) noexcept
{
	for(std::size_t node = 0; node < model.nodes.size(); ++node)
	{
		const int attachment = kind == EditorObjectKind::camera
			? model.nodes[node].camera : model.nodes[node].light;
		if(attachment == static_cast<int>(objectIndex))
			return stableIdAt(plan.idsByKind[kindIndex(EditorObjectKind::node)],
				static_cast<int>(node));
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<StableSceneId> materialId(
	const Plan& plan, int index) noexcept
{
	return stableIdAt(plan.idsByKind[kindIndex(EditorObjectKind::material)], index);
}

[[nodiscard]] std::optional<StableSceneId> variantMaterial(
	const tinygltf::Primitive& primitive, const Plan& plan,
	std::uint32_t variant) noexcept
{
	const auto extension = primitive.extensions.find("KHR_materials_variants");
	if(extension == primitive.extensions.end() || !extension->second.IsObject()
		|| !extension->second.Has("mappings")
		|| !extension->second.Get("mappings").IsArray()) return std::nullopt;
	for(const tinygltf::Value& mapping :
		extension->second.Get("mappings").Get<tinygltf::Value::Array>())
	{
		if(!mapping.IsObject() || !mapping.Has("material")
			|| !mapping.Get("material").IsInt() || !mapping.Has("variants")
			|| !mapping.Get("variants").IsArray()) continue;
		const auto& variants = mapping.Get("variants").Get<tinygltf::Value::Array>();
		if(std::any_of(variants.begin(), variants.end(), [&](const tinygltf::Value& item)
			{ return item.IsInt() && item.Get<int>() == static_cast<int>(variant); }))
			return materialId(plan, mapping.Get("material").Get<int>());
	}
	return std::nullopt;
}

[[nodiscard]] std::vector<const tinygltf::Primitive*> flatPrimitives(
	const tinygltf::Model& model)
{
	std::vector<const tinygltf::Primitive*> result;
	for(const tinygltf::Mesh& mesh : model.meshes)
		for(const tinygltf::Primitive& primitive : mesh.primitives)
			result.push_back(&primitive);
	return result;
}

[[nodiscard]] std::size_t variantCount(const tinygltf::Model& model) noexcept
{
	const auto extension = model.extensions.find("KHR_materials_variants");
	if(extension == model.extensions.end() || !extension->second.IsObject()
		|| !extension->second.Has("variants")
		|| !extension->second.Get("variants").IsArray()) return 0u;
	return extension->second.Get("variants").Get<tinygltf::Value::Array>().size();
}

[[nodiscard]] bool structuralDifference(const tinygltf::Model& before,
	const tinygltf::Model& after, const Plan& beforePlan, const Plan& afterPlan)
{
	if(beforePlan.hierarchyChanged || afterPlan.hierarchyChanged) return true;
	for(std::size_t kind = 1; kind < kindIndex(EditorObjectKind::count); ++kind)
		if(beforePlan.idsByKind[kind] != afterPlan.idsByKind[kind]) return true;
	if(before.scenes.size() != after.scenes.size() || before.nodes.size() != after.nodes.size()
		|| before.meshes.size() != after.meshes.size()) return true;
	for(std::size_t scene = 0; scene < before.scenes.size(); ++scene)
		if(before.scenes[scene].nodes != after.scenes[scene].nodes) return true;
	for(std::size_t node = 0; node < before.nodes.size(); ++node)
		if(before.nodes[node].children != after.nodes[node].children
			|| before.nodes[node].mesh != after.nodes[node].mesh
			|| before.nodes[node].skin != after.nodes[node].skin) return true;
	for(std::size_t mesh = 0; mesh < before.meshes.size(); ++mesh)
	{
		if(before.meshes[mesh].primitives.size() != after.meshes[mesh].primitives.size())
			return true;
		for(std::size_t primitive = 0; primitive < before.meshes[mesh].primitives.size(); ++primitive)
		{
			const auto& left = before.meshes[mesh].primitives[primitive];
			const auto& right = after.meshes[mesh].primitives[primitive];
			if(left.attributes != right.attributes || left.indices != right.indices
				|| left.mode != right.mode || left.targets != right.targets) return true;
		}
	}
	return false;
}

void populateExactEvidence(MutationResult& result,
	const tinygltf::Model& before, const tinygltf::Model& after,
	const Plan& beforePlan, const Plan& afterPlan,
	EditorDocumentAuthority authority, std::uint64_t baseRevision,
	std::uint64_t targetRevision, std::uint64_t serial,
	const SceneDigest* cachedBaseDigest = nullptr)
{
	result.changes = {};
	result.fullSnapshotReason.reset();
	const SceneDigest baseDigest = cachedBaseDigest ? *cachedBaseDigest
		: detail::SceneCommandsAccess::documentDigest(before);
	const SceneDigest targetDigest = detail::SceneCommandsAccess::documentDigest(after);
	if(!sceneDigestPresent(baseDigest) || !sceneDigestPresent(targetDigest))
		throw std::runtime_error("document digest capture failed");
	const MutationToken token{authority, targetRevision, serial};
	result.changes.sourceMutation = SourceMutationFence{
		baseRevision, targetRevision, sceneMutationToken(token), baseDigest, targetDigest};
	for(std::size_t kind = 1; kind < kindIndex(EditorObjectKind::count); ++kind)
		addExactRemap(result.changes, beforePlan.idsByKind[kind],
			afterPlan.idsByKind[kind]);

	const auto& beforeNodes = beforePlan.idsByKind[kindIndex(EditorObjectKind::node)];
	const auto& afterNodes = afterPlan.idsByKind[kindIndex(EditorObjectKind::node)];
	for(std::size_t index = 0; index < std::min(before.nodes.size(), after.nodes.size()); ++index)
	{
		if(index >= beforeNodes.size() || index >= afterNodes.size()
			|| beforeNodes[index] != afterNodes[index]) continue;
		const StableSceneId target = stableSceneId(afterNodes[index]);
		addUpdate(result.changes, target, SceneChangeClass::Transform,
			SceneField::NodeLocalTransform, serial,
			nodeTransformValue(before.nodes[index]), nodeTransformValue(after.nodes[index]));
		addUpdate(result.changes, target, SceneChangeClass::Visibility,
			SceneField::NodeLocalVisibility, serial,
			SceneVisibilityValue{nodeLocalVisibility(before.nodes[index])},
			SceneVisibilityValue{nodeLocalVisibility(after.nodes[index])});
	}

	const auto& beforeMaterials = beforePlan.idsByKind[kindIndex(EditorObjectKind::material)];
	const auto& afterMaterials = afterPlan.idsByKind[kindIndex(EditorObjectKind::material)];
	for(std::size_t index = 0; index < std::min(before.materials.size(), after.materials.size()); ++index)
	{
		if(index >= beforeMaterials.size() || index >= afterMaterials.size()
			|| beforeMaterials[index] != afterMaterials[index]) continue;
		const StableSceneId target = stableSceneId(afterMaterials[index]);
		const auto& left = before.materials[index];
		const auto& right = after.materials[index];
		addUpdate(result.changes, target, SceneChangeClass::MaterialParameters,
			SceneField::MaterialParameters, serial,
			materialParameters(left), materialParameters(right));
		struct TextureSource final
		{
			SceneTextureSemantic semantic;
			int beforeIndex;
			int afterIndex;
			int beforeUv;
			int afterUv;
			double beforeScalar;
			double afterScalar;
		};
		const std::array<TextureSource, 5> textures{{
			{SceneTextureSemantic::BaseColor,
				left.pbrMetallicRoughness.baseColorTexture.index,
				right.pbrMetallicRoughness.baseColorTexture.index,
				left.pbrMetallicRoughness.baseColorTexture.texCoord,
				right.pbrMetallicRoughness.baseColorTexture.texCoord, 1.0, 1.0},
			{SceneTextureSemantic::MetallicRoughness,
				left.pbrMetallicRoughness.metallicRoughnessTexture.index,
				right.pbrMetallicRoughness.metallicRoughnessTexture.index,
				left.pbrMetallicRoughness.metallicRoughnessTexture.texCoord,
				right.pbrMetallicRoughness.metallicRoughnessTexture.texCoord, 1.0, 1.0},
			{SceneTextureSemantic::Normal, left.normalTexture.index,
				right.normalTexture.index, left.normalTexture.texCoord,
				right.normalTexture.texCoord, left.normalTexture.scale,
				right.normalTexture.scale},
			{SceneTextureSemantic::Occlusion, left.occlusionTexture.index,
				right.occlusionTexture.index, left.occlusionTexture.texCoord,
				right.occlusionTexture.texCoord, left.occlusionTexture.strength,
				right.occlusionTexture.strength},
			{SceneTextureSemantic::Emissive, left.emissiveTexture.index,
				right.emissiveTexture.index, left.emissiveTexture.texCoord,
				right.emissiveTexture.texCoord, 1.0, 1.0},
		}};
		for(const TextureSource& texture : textures)
			addUpdate(result.changes, target, SceneChangeClass::MaterialBinding,
				SceneField::MaterialTextureBinding, serial,
				textureBinding(texture.semantic, texture.beforeIndex, texture.beforeUv,
					texture.beforeScalar, before, beforePlan),
				textureBinding(texture.semantic, texture.afterIndex, texture.afterUv,
					texture.afterScalar, after, afterPlan));
		std::set<std::string> extensionNames;
		for(const auto& [name, value] : left.extensions)
		{
			(void)value;
			extensionNames.insert(name);
		}
		for(const auto& [name, value] : right.extensions)
		{
			(void)value;
			extensionNames.insert(name);
		}
		for(const std::string& name : extensionNames)
		{
			const auto oldValue = left.extensions.find(name);
			const auto newValue = right.extensions.find(name);
			const tinygltf::Value nullValue;
			addUpdate(result.changes, target, SceneChangeClass::MaterialParameters,
				SceneField::MaterialExtension, serial,
				materialExtension(name, oldValue != left.extensions.end()
					? oldValue->second : nullValue),
				materialExtension(name, newValue != right.extensions.end()
					? newValue->second : nullValue));
		}
	}

	const auto& beforeCameras = beforePlan.idsByKind[kindIndex(EditorObjectKind::camera)];
	const auto& afterCameras = afterPlan.idsByKind[kindIndex(EditorObjectKind::camera)];
	for(std::size_t index = 0; index < std::min(before.cameras.size(), after.cameras.size()); ++index)
	{
		if(index >= beforeCameras.size() || index >= afterCameras.size()
			|| beforeCameras[index] != afterCameras[index]) continue;
		const StableSceneId target = stableSceneId(afterCameras[index]);
		addUpdate(result.changes, target, SceneChangeClass::Camera,
			SceneField::CameraParameters, serial,
			cameraValue(before.cameras[index]), cameraValue(after.cameras[index]));
		addUpdate(result.changes, target, SceneChangeClass::Camera,
			SceneField::CameraAttachment, serial,
			SceneObjectAttachmentValue{attachedNode(before, beforePlan,
				EditorObjectKind::camera, index)},
			SceneObjectAttachmentValue{attachedNode(after, afterPlan,
				EditorObjectKind::camera, index)});
	}

	const auto& beforeLights = beforePlan.idsByKind[kindIndex(EditorObjectKind::light)];
	const auto& afterLights = afterPlan.idsByKind[kindIndex(EditorObjectKind::light)];
	for(std::size_t index = 0; index < std::min(before.lights.size(), after.lights.size()); ++index)
	{
		if(index >= beforeLights.size() || index >= afterLights.size()
			|| beforeLights[index] != afterLights[index]) continue;
		const StableSceneId target = stableSceneId(afterLights[index]);
		addUpdate(result.changes, target, SceneChangeClass::Light,
			SceneField::LightParameters, serial,
			lightValue(before.lights[index]), lightValue(after.lights[index]));
		addUpdate(result.changes, target, SceneChangeClass::Light,
			SceneField::LightAttachment, serial,
			SceneObjectAttachmentValue{attachedNode(before, beforePlan,
				EditorObjectKind::light, index)},
			SceneObjectAttachmentValue{attachedNode(after, afterPlan,
				EditorObjectKind::light, index)});
	}

	const auto beforePrimitives = flatPrimitives(before);
	const auto afterPrimitives = flatPrimitives(after);
	const auto& beforePrimitiveIds = beforePlan.idsByKind[kindIndex(EditorObjectKind::primitive)];
	const auto& afterPrimitiveIds = afterPlan.idsByKind[kindIndex(EditorObjectKind::primitive)];
	for(std::size_t index = 0; index < std::min(beforePrimitives.size(), afterPrimitives.size()); ++index)
	{
		if(index >= beforePrimitiveIds.size() || index >= afterPrimitiveIds.size()
			|| beforePrimitiveIds[index] != afterPrimitiveIds[index]) continue;
		const StableSceneId target = stableSceneId(afterPrimitiveIds[index]);
		addUpdate(result.changes, target, SceneChangeClass::MaterialBinding,
			SceneField::PrimitiveMaterialBinding, serial,
			ScenePrimitiveMaterialBindingValue{
				materialId(beforePlan, beforePrimitives[index]->material), std::nullopt},
			ScenePrimitiveMaterialBindingValue{
				materialId(afterPlan, afterPrimitives[index]->material), std::nullopt});
		const std::size_t variants = std::max(variantCount(before), variantCount(after));
		for(std::size_t variant = 0; variant < variants; ++variant)
		{
			const auto oldMaterial = variantMaterial(
				*beforePrimitives[index], beforePlan, static_cast<std::uint32_t>(variant));
			const auto newMaterial = variantMaterial(
				*afterPrimitives[index], afterPlan, static_cast<std::uint32_t>(variant));
			if(oldMaterial == newMaterial) continue;
			addUpdate(result.changes, target, SceneChangeClass::MaterialBinding,
				SceneField::PrimitiveMaterialBinding, serial,
				ScenePrimitiveMaterialBindingValue{oldMaterial,
					static_cast<std::uint32_t>(variant)},
				ScenePrimitiveMaterialBindingValue{newMaterial,
					static_cast<std::uint32_t>(variant)});
		}
	}

	if(structuralDifference(before, after, beforePlan, afterPlan))
	{
		result.changes.structuralEvidenceComplete = false;
		result.fullSnapshotReason = FullRebuildReason::StructuralTopology;
	}
	std::vector<EditorObjectId> changed;
	changed.reserve(result.changes.exactChanges.size()
		+ result.changes.stableIdRemap.size() * 2u);
	for(const SceneFieldChange& change : result.changes.exactChanges)
		changed.push_back(editorObjectId(change.target));
	for(const StableIdRemapEvidence& remap : result.changes.stableIdRemap)
	{
		if(remap.oldId) changed.push_back(editorObjectId(*remap.oldId));
		if(remap.newId) changed.push_back(editorObjectId(*remap.newId));
	}
	std::sort(changed.begin(), changed.end(), [](EditorObjectId left, EditorObjectId right)
		{ return stableKey(left) < stableKey(right); });
	changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
	result.changedIds = std::move(changed);
}

void clearPublishedEvidence(MutationResult& result) noexcept
{
	result.changedIds.clear();
	result.remap = {};
	result.changes = {};
	result.fullSnapshotReason.reset();
	result.sceneActivationTransition.reset();
	result.rootStateTransition.reset();
}

MutationResult baseResult(
	HistoryPolicy historyPolicy,
	const GltfDocument& document, const MutationGuard& guard, std::uint64_t serial)
{
	MutationResult result;
	result.historyPolicy = historyPolicy;
	result.beforeRevision = document.revision();
	result.afterRevision = document.revision();
	result.transactionToken = MutationToken{document.documentAuthority(), document.revision(), serial};
	if(serial == 0)
	{
		result.error = MutationErrorCode::internalError;
		result.message = "mutation serial space is exhausted";
	}
	else if(guard.expectedRevision && *guard.expectedRevision != document.revision())
	{
		result.error = MutationErrorCode::staleRevision;
		result.message = "mutation expected a different document revision";
	}
	else if(guard.expectedToken
		&& (guard.expectedToken->authority != document.documentAuthority()
			|| guard.expectedToken->revision != document.revision()
			|| guard.expectedToken->serial != serial
			|| !guard.expectedToken->valid()))
	{
		result.error = guard.expectedToken->authority != document.documentAuthority()
			? MutationErrorCode::crossDocument : MutationErrorCode::staleTransaction;
		result.message = "mutation token is stale or belongs to another document";
	}
	else
	{
		result.error = MutationErrorCode::none;
		result.message.clear();
	}
	return result;
}

std::vector<std::size_t> occurrences(const tinygltf::Model& model, int node)
{
	std::vector<std::size_t> result;
	for(std::size_t scene = 0; scene < model.scenes.size(); ++scene)
	{
		std::vector<int> stack(model.scenes[scene].nodes.rbegin(), model.scenes[scene].nodes.rend());
		std::unordered_set<int> visited;
		while(!stack.empty())
		{
			const int current = stack.back();
			stack.pop_back();
			if(!visited.insert(current).second) continue;
			if(current == node) result.push_back(scene);
			if(current >= 0 && static_cast<std::size_t>(current) < model.nodes.size())
			{
				for(auto child = model.nodes[static_cast<std::size_t>(current)].children.rbegin();
					child != model.nodes[static_cast<std::size_t>(current)].children.rend(); ++child)
				{
					stack.push_back(*child);
				}
			}
		}
	}
	return result;
}

bool hasNodeParent(const tinygltf::Model& model, int node, int* parent = nullptr)
{
	bool found = false;
	for(std::size_t index = 0; index < model.nodes.size(); ++index)
	{
		const auto& children = model.nodes[index].children;
		if(std::find(children.begin(), children.end(), node) != children.end())
		{
			if(parent) *parent = static_cast<int>(index);
			if(found) return true;
			found = true;
		}
	}
	return found;
}

std::size_t nodeParentCount(const tinygltf::Model& model, int node)
{
	std::size_t count = 0;
	for(const auto& candidate : model.nodes)
		count += static_cast<std::size_t>(std::count(candidate.children.begin(), candidate.children.end(), node));
	for(const auto& scene : model.scenes)
		count += static_cast<std::size_t>(std::count(scene.nodes.begin(), scene.nodes.end(), node));
	return count;
}

bool reachable(const tinygltf::Model& model, int root, int target)
{
	std::vector<int> stack{root};
	std::unordered_set<int> visited;
	while(!stack.empty())
	{
		const int current = stack.back();
		stack.pop_back();
		if(!visited.insert(current).second) continue;
		if(current == target) return true;
		if(current < 0 || static_cast<std::size_t>(current) >= model.nodes.size()) continue;
		for(const int child : model.nodes[static_cast<std::size_t>(current)].children) stack.push_back(child);
	}
	return false;
}

void markSubtree(const tinygltf::Model& model, int root, std::vector<bool>& deleted)
{
	std::vector<int> stack{root};
	while(!stack.empty())
	{
		const int current = stack.back();
		stack.pop_back();
		if(current < 0 || static_cast<std::size_t>(current) >= deleted.size()
			|| deleted[static_cast<std::size_t>(current)]) continue;
		deleted[static_cast<std::size_t>(current)] = true;
		for(const int child : model.nodes[static_cast<std::size_t>(current)].children) stack.push_back(child);
	}
}

struct SceneStructureSnapshot final
{
	std::vector<std::vector<int>> sceneRoots;
	std::vector<std::vector<int>> nodeChildren;
	std::vector<std::vector<std::size_t>> scenesByNode;
	std::vector<int> nodeParent;
};

struct TraversalItem final
{
	int node{-1};
	std::size_t depth{0};
};

[[nodiscard]] bool containsScene(
	const std::vector<std::size_t>& scenes, std::size_t scene) noexcept
{
	return std::binary_search(scenes.begin(), scenes.end(), scene);
}

[[nodiscard]] MutationErrorCode captureSceneStructure(
	const tinygltf::Model& model,
	const EditorLimits& limits,
	SceneStructureSnapshot& snapshot)
{
	snapshot.sceneRoots.clear();
	snapshot.nodeChildren.clear();
	snapshot.scenesByNode.assign(model.nodes.size(), {});
	snapshot.nodeParent.assign(model.nodes.size(), -1);
	snapshot.sceneRoots.reserve(model.scenes.size());
	for(const auto& scene : model.scenes) snapshot.sceneRoots.push_back(scene.nodes);
	snapshot.nodeChildren.reserve(model.nodes.size());

	std::size_t edgeCount = 0;
	for(std::size_t parent = 0; parent < model.nodes.size(); ++parent)
	{
		const auto& children = model.nodes[parent].children;
		snapshot.nodeChildren.push_back(children);
		if(edgeCount > limits.maxNodeEdges
			|| children.size() > limits.maxNodeEdges - edgeCount)
			return MutationErrorCode::limitExceeded;
		edgeCount += children.size();
		for(const int child : children)
		{
			if(child < 0 || static_cast<std::size_t>(child) >= model.nodes.size())
				return MutationErrorCode::invalidReference;
			int& recordedParent = snapshot.nodeParent[static_cast<std::size_t>(child)];
			if(recordedParent >= 0) return MutationErrorCode::multiParent;
			recordedParent = static_cast<int>(parent);
		}
	}

	std::vector<std::uint32_t> sceneMarks(model.nodes.size(), 0u);
	std::uint32_t sceneMark = 0u;
	std::size_t visits = 0u;
	std::vector<TraversalItem> stack;
	stack.reserve(model.nodes.size());
	for(std::size_t scene = 0; scene < model.scenes.size(); ++scene)
	{
		++sceneMark;
		stack.clear();
		for(auto root = model.scenes[scene].nodes.rbegin();
			root != model.scenes[scene].nodes.rend(); ++root)
		{
			stack.push_back(TraversalItem{*root, 1u});
		}
		while(!stack.empty())
		{
			const TraversalItem current = stack.back();
			stack.pop_back();
			if(current.node < 0 || static_cast<std::size_t>(current.node) >= model.nodes.size())
				return MutationErrorCode::invalidReference;
			if(current.depth > limits.maxNodeDepth
				|| visits >= limits.maxSceneNodeOccurrences)
				return MutationErrorCode::limitExceeded;
			++visits;
			const std::size_t node = static_cast<std::size_t>(current.node);
			if(sceneMarks[node] == sceneMark) return MutationErrorCode::multiParent;
			sceneMarks[node] = sceneMark;
			snapshot.scenesByNode[node].push_back(scene);
			for(auto child = snapshot.nodeChildren[node].rbegin();
				child != snapshot.nodeChildren[node].rend(); ++child)
			{
				stack.push_back(TraversalItem{*child, current.depth + 1u});
			}
		}
	}
	return MutationErrorCode::none;
}

[[nodiscard]] std::optional<int> occurrenceParent(
	const SceneStructureSnapshot& snapshot,
	std::size_t scene,
	std::size_t node) noexcept
{
	if(scene >= snapshot.sceneRoots.size() || node >= snapshot.scenesByNode.size()
		|| !containsScene(snapshot.scenesByNode[node], scene)) return std::nullopt;
	const int parent = snapshot.nodeParent[node];
	if(parent >= 0 && static_cast<std::size_t>(parent) < snapshot.scenesByNode.size()
		&& containsScene(snapshot.scenesByNode[static_cast<std::size_t>(parent)], scene))
	{
		return parent;
	}
	const auto& roots = snapshot.sceneRoots[scene];
	return std::find(roots.begin(), roots.end(), static_cast<int>(node)) != roots.end()
		? std::optional<int>{-1} : std::nullopt;
}

[[nodiscard]] MutationErrorCode collectSubtree(
	const SceneStructureSnapshot& snapshot,
	std::size_t root,
	const EditorLimits& limits,
	std::vector<std::size_t>& nodes)
{
	nodes.clear();
	if(root >= snapshot.nodeChildren.size()) return MutationErrorCode::invalidReference;
	std::vector<std::uint8_t> visited(snapshot.nodeChildren.size(), 0u);
	std::vector<TraversalItem> stack{TraversalItem{static_cast<int>(root), 1u}};
	while(!stack.empty())
	{
		const TraversalItem current = stack.back();
		stack.pop_back();
		if(current.node < 0 || static_cast<std::size_t>(current.node) >= visited.size())
			return MutationErrorCode::invalidReference;
		if(current.depth > limits.maxNodeDepth
			|| nodes.size() >= limits.maxSceneNodeOccurrences)
			return MutationErrorCode::limitExceeded;
		const std::size_t node = static_cast<std::size_t>(current.node);
		if(visited[node] != 0u) continue;
		visited[node] = 1u;
		nodes.push_back(node);
		for(auto child = snapshot.nodeChildren[node].rbegin();
			child != snapshot.nodeChildren[node].rend(); ++child)
		{
			stack.push_back(TraversalItem{*child, current.depth + 1u});
		}
	}
	std::sort(nodes.begin(), nodes.end());
	return MutationErrorCode::none;
}

[[nodiscard]] MutationErrorCode boundedReachable(
	const SceneStructureSnapshot& snapshot,
	std::size_t root,
	std::size_t target,
	const EditorLimits& limits,
	bool& result)
{
	result = false;
	std::vector<std::size_t> nodes;
	const MutationErrorCode error = collectSubtree(snapshot, root, limits, nodes);
	if(error == MutationErrorCode::none)
		result = std::binary_search(nodes.begin(), nodes.end(), target);
	return error;
}

[[nodiscard]] MutationErrorCode cloneNodes(
	tinygltf::Model& model,
	Plan& plan,
	const SceneStructureSnapshot& snapshot,
	const std::vector<std::size_t>& oldNodes,
	const EditorLimits& limits,
	std::vector<int>& nodeMap)
{
	nodeMap.assign(snapshot.nodeChildren.size(), -1);
	if(oldNodes.empty()) return MutationErrorCode::none;
	if(model.nodes.size() > limits.maxNodes
		|| oldNodes.size() > limits.maxNodes - model.nodes.size())
		return MutationErrorCode::limitExceeded;
	const std::size_t firstNode = model.nodes.size();
	for(std::size_t offset = 0; offset < oldNodes.size(); ++offset)
	{
		if(oldNodes[offset] >= nodeMap.size()) return MutationErrorCode::invalidReference;
		nodeMap[oldNodes[offset]] = static_cast<int>(firstNode + offset);
	}

	std::vector<std::size_t> oldSkins;
	std::vector<int> skinMap(model.skins.size(), -1);
	for(const std::size_t node : oldNodes)
	{
		const int skin = model.nodes[node].skin;
		if(skin < 0) continue;
		if(static_cast<std::size_t>(skin) >= model.skins.size())
			return MutationErrorCode::invalidReference;
		if(skinMap[static_cast<std::size_t>(skin)] < 0)
		{
			skinMap[static_cast<std::size_t>(skin)] = 0;
			oldSkins.push_back(static_cast<std::size_t>(skin));
		}
	}
	std::sort(oldSkins.begin(), oldSkins.end());
	if(model.skins.size() > limits.maxSkins
		|| oldSkins.size() > limits.maxSkins - model.skins.size())
		return MutationErrorCode::limitExceeded;
	for(std::size_t offset = 0; offset < oldSkins.size(); ++offset)
		skinMap[oldSkins[offset]] = static_cast<int>(model.skins.size() + offset);

	std::size_t channelCount = 0u;
	std::size_t addedChannels = 0u;
	for(const auto& animation : model.animations)
	{
		if(animation.channels.size() > limits.maxAnimationChannels
			- std::min(limits.maxAnimationChannels, channelCount))
			return MutationErrorCode::limitExceeded;
		channelCount += animation.channels.size();
		for(const auto& channel : animation.channels)
		{
			if(channel.target_node >= 0
				&& static_cast<std::size_t>(channel.target_node) < nodeMap.size()
				&& nodeMap[static_cast<std::size_t>(channel.target_node)] >= 0)
			{
				++addedChannels;
			}
		}
	}
	if(channelCount > limits.maxAnimationChannels
		|| addedChannels > limits.maxAnimationChannels - channelCount)
		return MutationErrorCode::limitExceeded;

	model.skins.reserve(model.skins.size() + oldSkins.size());
	for(const std::size_t oldSkin : oldSkins)
	{
		tinygltf::Skin copy = model.skins[oldSkin];
		for(int& joint : copy.joints)
		{
			if(joint >= 0 && static_cast<std::size_t>(joint) < nodeMap.size()
				&& nodeMap[static_cast<std::size_t>(joint)] >= 0)
				joint = nodeMap[static_cast<std::size_t>(joint)];
		}
		if(copy.skeleton >= 0 && static_cast<std::size_t>(copy.skeleton) < nodeMap.size()
			&& nodeMap[static_cast<std::size_t>(copy.skeleton)] >= 0)
			copy.skeleton = nodeMap[static_cast<std::size_t>(copy.skeleton)];
		model.skins.push_back(std::move(copy));
		plan.idsByKind[kindIndex(EditorObjectKind::skin)].push_back(EditorObjectId{});
	}

	model.nodes.reserve(model.nodes.size() + oldNodes.size());
	for(const std::size_t oldNode : oldNodes)
	{
		tinygltf::Node copy = model.nodes[oldNode];
		for(int& child : copy.children)
		{
			if(child >= 0 && static_cast<std::size_t>(child) < nodeMap.size()
				&& nodeMap[static_cast<std::size_t>(child)] >= 0)
				child = nodeMap[static_cast<std::size_t>(child)];
		}
		if(copy.skin >= 0 && static_cast<std::size_t>(copy.skin) < skinMap.size()
			&& skinMap[static_cast<std::size_t>(copy.skin)] >= 0)
			copy.skin = skinMap[static_cast<std::size_t>(copy.skin)];
		model.nodes.push_back(std::move(copy));
	}
	plan.idsByKind[kindIndex(EditorObjectKind::node)].insert(
		plan.idsByKind[kindIndex(EditorObjectKind::node)].end(),
		oldNodes.size(), EditorObjectId{});

	for(auto& animation : model.animations)
	{
		const std::size_t originalChannels = animation.channels.size();
		animation.channels.reserve(originalChannels + addedChannels);
		for(std::size_t index = 0; index < originalChannels; ++index)
		{
			const int target = animation.channels[index].target_node;
			if(target < 0 || static_cast<std::size_t>(target) >= nodeMap.size()
				|| nodeMap[static_cast<std::size_t>(target)] < 0) continue;
			tinygltf::AnimationChannel copy = animation.channels[index];
			copy.target_node = nodeMap[static_cast<std::size_t>(target)];
			animation.channels.push_back(std::move(copy));
		}
	}
	return MutationErrorCode::none;
}

[[nodiscard]] MutationErrorCode isolateClickedScene(
	tinygltf::Model& model,
	Plan& plan,
	const SceneStructureSnapshot& snapshot,
	std::size_t clickedScene,
	const EditorLimits& limits)
{
	std::vector<std::size_t> sharedNodes;
	for(std::size_t node = 0; node < snapshot.scenesByNode.size(); ++node)
	{
		const auto& scenes = snapshot.scenesByNode[node];
		if(scenes.size() > 1u && containsScene(scenes, clickedScene))
			sharedNodes.push_back(node);
	}
	std::vector<int> nodeMap;
	const MutationErrorCode cloneError = cloneNodes(
		model, plan, snapshot, sharedNodes, limits, nodeMap);
	if(cloneError != MutationErrorCode::none) return cloneError;
	if(sharedNodes.empty()) return MutationErrorCode::none;

	for(std::size_t scene = 0; scene < model.scenes.size(); ++scene)
	{
		if(scene == clickedScene) continue;
		for(int& root : model.scenes[scene].nodes)
		{
			if(root >= 0 && static_cast<std::size_t>(root) < nodeMap.size()
				&& nodeMap[static_cast<std::size_t>(root)] >= 0)
				root = nodeMap[static_cast<std::size_t>(root)];
		}
	}
	for(std::size_t node = 0; node < snapshot.scenesByNode.size(); ++node)
	{
		const auto& scenes = snapshot.scenesByNode[node];
		if(scenes.empty() || containsScene(scenes, clickedScene)) continue;
		for(int& child : model.nodes[node].children)
		{
			if(child >= 0 && static_cast<std::size_t>(child) < nodeMap.size()
				&& nodeMap[static_cast<std::size_t>(child)] >= 0)
				child = nodeMap[static_cast<std::size_t>(child)];
		}
	}
	return MutationErrorCode::none;
}

void eraseDeletedNodes(tinygltf::Model& model, Plan& plan, const std::vector<bool>& deleted)
{
	std::vector<int> map(model.nodes.size(), -1);
	int next = 0;
	for(std::size_t old = 0; old < model.nodes.size(); ++old)
		if(old >= deleted.size() || !deleted[old]) map[old] = next++;
	std::vector<tinygltf::Node> nodes;
	nodes.reserve(static_cast<std::size_t>(next));
	for(std::size_t old = 0; old < model.nodes.size(); ++old)
	{
		if(old < deleted.size() && deleted[old]) continue;
		tinygltf::Node copy = model.nodes[old];
		remapIndices(copy.children, map);
		nodes.push_back(std::move(copy));
	}
	for(auto& scene : model.scenes) remapIndices(scene.nodes, map);

	std::vector<int> skinMap(model.skins.size(), -1);
	std::vector<tinygltf::Skin> skins;
	std::vector<EditorObjectId> skinIds;
	skins.reserve(model.skins.size());
	skinIds.reserve(model.skins.size());
	const auto oldSkinIds = plan.idsByKind[kindIndex(EditorObjectKind::skin)];
	for(std::size_t old = 0; old < model.skins.size(); ++old)
	{
		tinygltf::Skin copy = model.skins[old];
		remapIndices(copy.joints, map);
		remapIndex(copy.skeleton, map);
		if(copy.joints.empty()) continue;
		skinMap[old] = static_cast<int>(skins.size());
		skins.push_back(std::move(copy));
		skinIds.push_back(oldSkinIds[old]);
	}
	for(auto& node : nodes) remapIndex(node.skin, skinMap);
	model.skins.swap(skins);
	plan.idsByKind[kindIndex(EditorObjectKind::skin)] = std::move(skinIds);

	std::vector<tinygltf::Animation> animations;
	std::vector<EditorObjectId> animationIds;
	animations.reserve(model.animations.size());
	animationIds.reserve(model.animations.size());
	const auto oldAnimationIds = plan.idsByKind[kindIndex(EditorObjectKind::animation)];
	for(std::size_t old = 0; old < model.animations.size(); ++old)
	{
		tinygltf::Animation copy = model.animations[old];
		copy.channels.erase(std::remove_if(copy.channels.begin(), copy.channels.end(),
			[&](const tinygltf::AnimationChannel& channel)
			{
				return channel.target_node >= 0
					&& static_cast<std::size_t>(channel.target_node) < deleted.size()
					&& deleted[static_cast<std::size_t>(channel.target_node)];
			}), copy.channels.end());
		for(auto& channel : copy.channels) remapIndex(channel.target_node, map);
		if(copy.channels.empty()) continue;
		animations.push_back(std::move(copy));
		animationIds.push_back(oldAnimationIds[old]);
	}
	model.animations.swap(animations);
	plan.idsByKind[kindIndex(EditorObjectKind::animation)] = std::move(animationIds);

	model.nodes.swap(nodes);
	const auto oldNodeIds = plan.idsByKind[kindIndex(EditorObjectKind::node)];
	std::vector<EditorObjectId> nextNodeIds;
	nextNodeIds.reserve(model.nodes.size());
	for(std::size_t old = 0; old < map.size(); ++old)
		if(map[old] >= 0) nextNodeIds.push_back(oldNodeIds[old]);
	plan.idsByKind[kindIndex(EditorObjectKind::node)] = std::move(nextNodeIds);
}

[[nodiscard]] MutationErrorCode markSceneReachableNodes(
	const tinygltf::Model& model,
	const EditorLimits& limits,
	std::vector<bool>& reachable)
{
	reachable.assign(model.nodes.size(), false);
	std::vector<TraversalItem> stack;
	stack.reserve(model.nodes.size());
	std::size_t visits = 0u;
	for(const auto& scene : model.scenes)
	{
		for(auto root = scene.nodes.rbegin(); root != scene.nodes.rend(); ++root)
			stack.push_back(TraversalItem{*root, 1u});
		while(!stack.empty())
		{
			const TraversalItem current = stack.back();
			stack.pop_back();
			if(current.node < 0 || static_cast<std::size_t>(current.node) >= model.nodes.size())
				return MutationErrorCode::invalidReference;
			if(current.depth > limits.maxNodeDepth
				|| visits >= limits.maxSceneNodeOccurrences)
				return MutationErrorCode::limitExceeded;
			++visits;
			const std::size_t node = static_cast<std::size_t>(current.node);
			if(reachable[node]) continue;
			reachable[node] = true;
			for(auto child = model.nodes[node].children.rbegin();
				child != model.nodes[node].children.rend(); ++child)
			{
				stack.push_back(TraversalItem{*child, current.depth + 1u});
			}
		}
	}
	return MutationErrorCode::none;
}

template <typename Mutator>
MutationResult mutate(GltfDocument& document,
	                  HistoryPolicy historyPolicy,
	                  const MutationGuard& guard,
	                  EditorLimits limits,
	                  EditorFaultInjector* faultInjector,
	                  std::uint64_t& serial,
	                  Mutator&& mutator)
{
	MutationResult result = baseResult(historyPolicy, document, guard, serial);
	if(result.error != MutationErrorCode::none)
	{
		return result;
	}
	if(document.revision() == std::numeric_limits<std::uint64_t>::max())
	{
		result.error = MutationErrorCode::revisionExhausted;
		result.message = "document revision is exhausted";
		return result;
	}
	try
	{
		if(faultInjector != nullptr && faultInjector->shouldFail(EditorFaultPoint::allocation))
		{
			result.error = MutationErrorCode::allocationFailed;
			result.message = "mutation allocation fault was injected before candidate creation";
			return result;
		}
		const Plan planBefore = makePlan(document);
		Plan plan = planBefore;
		auto candidate = std::make_unique<tinygltf::Model>(
			detail::SceneCommandsAccess::model(document));
		const MutationErrorCode mutationError = mutator(*candidate, plan, result.message);
		if(mutationError == MutationErrorCode::noOp
			|| mutationError == MutationErrorCode::uniqueRootDeleteNoop)
		{
			result.status = MutationStatus::domainNoop;
			result.error = mutationError;
			result.message = mutationError == MutationErrorCode::uniqueRootDeleteNoop
				? "deleting the unique scene root is a document no-op" : "operation has no document effect";
			return result;
		}
		if(mutationError != MutationErrorCode::none)
		{
			result.error = mutationError;
			if(result.message.empty()) result.message = "mutation preflight rejected";
			return result;
		}
		if(plan.hierarchyChanged)
		{
			const MutationErrorCode bakeError = bakeSceneRootPreviews(document, *candidate, plan);
			if(bakeError != MutationErrorCode::none)
			{
				result.error = bakeError;
				result.message = "scene-root preview cannot be frozen consistently across scene occurrences";
				return result;
			}
		}
		const GltfValidationReport validation = detail::SceneCommandsAccess::validateModel(*candidate, limits);
		result.validation = validation;
		if(!validation.valid())
		{
			result.error = MutationErrorCode::validationFailed;
			result.message = validation.issues().empty() ? "candidate failed validation" : validation.issues().front().message;
			return result;
		}
		if(faultInjector != nullptr && faultInjector->shouldFail(EditorFaultPoint::preCandidatePublish))
		{
			result.error = MutationErrorCode::internalError;
			result.message = "mutation publication fault was injected";
			return result;
		}
		const std::uint64_t nextSerial = allocateMutationSerial();
		if(nextSerial == 0)
		{
			result.error = MutationErrorCode::internalError;
			result.message = "mutation serial space is exhausted";
			return result;
		}
		struct PrePublishContext final
		{
			MutationResult* result;
			const Plan* before;
			const tinygltf::Model* beforeModel;
			const tinygltf::Model* afterModel;
			EditorFaultInjector* faultInjector;
			EditorDocumentAuthority authority;
			std::uint64_t baseRevision;
			std::uint64_t revision;
			std::uint64_t baseActivationEpoch;
			std::uint64_t serial;
			SceneDigest baseDigest;
		} context{&result, &planBefore,
			&detail::SceneCommandsAccess::model(document), candidate.get(),
			faultInjector, document.documentAuthority(), document.revision(),
			nextRevisionWithoutWrap(document.revision()),
			document.sceneActivationEpoch(), nextSerial, document.documentDigest()};
		const auto prepareResult = [](const Plan& resolved, void* opaque)
		{
			auto& value = *static_cast<PrePublishContext*>(opaque);
			if(value.faultInjector != nullptr
				&& value.faultInjector->shouldFail(EditorFaultPoint::allocation))
			{
				throw std::bad_alloc{};
			}
			value.result->status = MutationStatus::applied;
			value.result->error = MutationErrorCode::none;
			value.result->afterRevision = value.revision;
			value.result->transactionToken = MutationToken{
				value.authority, value.revision, value.serial};
			for(std::size_t kind = 1; kind < kindIndex(EditorObjectKind::count); ++kind)
			{
				const auto type = static_cast<EditorObjectKind>(kind);
				addRemap(*value.result, value.before->idsByKind[kind],
					resolved.idsByKind[kind], type);
			}
			populateExactEvidence(*value.result, *value.beforeModel, *value.afterModel,
				*value.before, resolved, value.authority, value.baseRevision,
				value.revision, value.serial, &value.baseDigest);
			if(value.before->currentScene != resolved.currentScene)
			{
				value.result->sceneActivationTransition = SceneActivationResult{
					SceneActivationStatus::Changed,
					value.before->currentScene.valid()
						? std::optional<SceneId>{value.before->currentScene} : std::nullopt,
					resolved.currentScene.valid()
						? std::optional<SceneId>{resolved.currentScene} : std::nullopt,
					value.baseActivationEpoch, value.baseActivationEpoch + 1u};
			}
		};
		detail::MutationCommitStatus commitStatus = detail::MutationCommitStatus::internalError;
		if(!detail::SceneCommandsAccess::commit(document, std::move(candidate), plan,
			prepareResult, &context, commitStatus))
		{
			result.status = MutationStatus::rejected;
			result.afterRevision = result.beforeRevision;
			clearPublishedEvidence(result);
			result.transactionToken = MutationToken{
				document.documentAuthority(), document.revision(), serial};
			switch(commitStatus)
			{
				case detail::MutationCommitStatus::allocationFailed:
					result.error = MutationErrorCode::allocationFailed;
					result.message = "mutation commit allocation failed before publication";
					break;
				case detail::MutationCommitStatus::generationExhausted:
					result.error = MutationErrorCode::generationExhausted;
					result.message = "object generation is exhausted";
					break;
				case detail::MutationCommitStatus::revisionExhausted:
					result.error = MutationErrorCode::revisionExhausted;
					result.message = "document revision is exhausted";
					break;
				case detail::MutationCommitStatus::sceneActivationEpochExhausted:
					result.error = MutationErrorCode::sceneActivationEpochExhausted;
					result.message = "scene activation epoch is exhausted";
					break;
				case detail::MutationCommitStatus::hierarchyEpochExhausted:
					result.error = MutationErrorCode::hierarchyEpochExhausted;
					result.message = "scene hierarchy epoch is exhausted";
					break;
				case detail::MutationCommitStatus::externalStateSerialExhausted:
					result.error = MutationErrorCode::externalStateSerialExhausted;
					result.message = "scene external-state serial is exhausted";
					break;
				default:
					result.error = MutationErrorCode::internalError;
					result.message = "mutation commit failed before publication";
					break;
			}
			return result;
		}
		serial = nextSerial;
		return result;
	}
	catch(const std::bad_alloc&)
	{
		result.error = MutationErrorCode::allocationFailed;
		result.message = "mutation allocation failed before commit";
		return result;
	}
	catch(...)
	{
		result.error = MutationErrorCode::internalError;
		result.message = "mutation failed before commit";
		return result;
	}
}

[[nodiscard]] MutationResult mutateNodeNameOnly(
	GltfDocument& document, NodeId node, std::string name,
	const MutationGuard& guard, const EditorLimits& limits,
	EditorFaultInjector* faultInjector, std::uint64_t& serial)
{
	MutationResult result = baseResult(
		HistoryPolicy::undoable, document, guard, serial);
	if(result.error != MutationErrorCode::none) return result;
	if(document.revision() == std::numeric_limits<std::uint64_t>::max())
	{
		result.error = MutationErrorCode::revisionExhausted;
		result.message = "document revision is exhausted";
		return result;
	}
	if(node.authority() != document.documentAuthority())
	{
		result.error = MutationErrorCode::crossDocument;
		result.message = "mutation preflight rejected";
		return result;
	}
	const auto nodeIndex = document.indexOf(node.untyped());
	if(!nodeIndex)
	{
		result.error = MutationErrorCode::staleId;
		result.message = "mutation preflight rejected";
		return result;
	}
	if(name.size() > limits.maxStringBytes)
	{
		result.error = MutationErrorCode::limitExceeded;
		result.message = "mutation preflight rejected";
		return result;
	}
	if(document.objectName(node.untyped()) == name)
	{
		result.status = MutationStatus::domainNoop;
		result.error = MutationErrorCode::noOp;
		result.message = "operation has no document effect";
		return result;
	}

	try
	{
		if(faultInjector != nullptr
			&& faultInjector->shouldFail(EditorFaultPoint::allocation))
		{
			result.error = MutationErrorCode::allocationFailed;
			result.message =
				"mutation allocation fault was injected before candidate creation";
			return result;
		}
		Plan plan = makePlan(document);
		const SceneDigest baseDigest = document.documentDigest();
		if(!sceneDigestPresent(baseDigest))
		{
			result.error = MutationErrorCode::internalError;
			result.message = "document digest capture failed";
			return result;
		}
		if(faultInjector != nullptr
			&& faultInjector->shouldFail(EditorFaultPoint::preCandidatePublish))
		{
			result.error = MutationErrorCode::internalError;
			result.message = "mutation publication fault was injected";
			return result;
		}
		const std::uint64_t nextSerial = allocateMutationSerial();
		if(nextSerial == 0)
		{
			result.error = MutationErrorCode::internalError;
			result.message = "mutation serial space is exhausted";
			return result;
		}
		struct PrePublishContext final
		{
			MutationResult* result;
			const Plan* before;
			GltfDocument* document;
			EditorFaultInjector* faultInjector;
			EditorDocumentAuthority authority;
			std::uint64_t baseRevision;
			std::uint64_t targetRevision;
			std::uint64_t serial;
			SceneDigest baseDigest;
		} context{&result, &plan, &document, faultInjector,
			document.documentAuthority(), document.revision(),
			nextRevisionWithoutWrap(document.revision()), nextSerial, baseDigest};
		const auto prepareResult = [](const Plan& resolved, void* opaque)
		{
			auto& value = *static_cast<PrePublishContext*>(opaque);
			if(value.faultInjector != nullptr
				&& value.faultInjector->shouldFail(EditorFaultPoint::allocation))
			{
				throw std::bad_alloc{};
			}
			const SceneDigest targetDigest = value.document->documentDigest();
			if(!sceneDigestPresent(targetDigest))
				throw std::runtime_error("document digest capture failed");
			value.result->status = MutationStatus::applied;
			value.result->error = MutationErrorCode::none;
			value.result->message.clear();
			value.result->afterRevision = value.targetRevision;
			value.result->transactionToken = MutationToken{
				value.authority, value.targetRevision, value.serial};
			value.result->changes = {};
			value.result->fullSnapshotReason.reset();
			value.result->changedIds.clear();
			for(std::size_t kind = 1;
				kind < kindIndex(EditorObjectKind::count); ++kind)
			{
				addRemap(*value.result, value.before->idsByKind[kind],
					resolved.idsByKind[kind],
					static_cast<EditorObjectKind>(kind));
				addExactRemap(value.result->changes,
					value.before->idsByKind[kind], resolved.idsByKind[kind]);
			}
			value.result->changes.sourceMutation = SourceMutationFence{
				value.baseRevision, value.targetRevision,
				sceneMutationToken(value.result->transactionToken),
				value.baseDigest, targetDigest};
		};

		const detail::MutationCommitStatus commitStatus =
			detail::SceneCommandsAccess::commitNodeName(
				document, *nodeIndex, std::move(name), plan,
				prepareResult, &context);
		if(commitStatus != detail::MutationCommitStatus::applied)
		{
			result.status = MutationStatus::rejected;
			result.afterRevision = result.beforeRevision;
			clearPublishedEvidence(result);
			result.transactionToken = MutationToken{
				document.documentAuthority(), document.revision(), serial};
			switch(commitStatus)
			{
				case detail::MutationCommitStatus::allocationFailed:
					result.error = MutationErrorCode::allocationFailed;
					result.message =
						"mutation commit allocation failed before publication";
					break;
				case detail::MutationCommitStatus::revisionExhausted:
					result.error = MutationErrorCode::revisionExhausted;
					result.message = "document revision is exhausted";
					break;
				default:
					result.error = MutationErrorCode::internalError;
					result.message = "mutation commit failed before publication";
					break;
			}
			return result;
		}
		serial = nextSerial;
		return result;
	}
	catch(const std::bad_alloc&)
	{
		result.error = MutationErrorCode::allocationFailed;
		result.message = "mutation allocation failed before commit";
		return result;
	}
	catch(...)
	{
		result.error = MutationErrorCode::internalError;
		result.message = "mutation failed before commit";
		return result;
	}
}

struct SnapshotStableReference final
{
	EditorObjectKind kind{EditorObjectKind::invalid};
	std::int32_t sourceIndex{-1};
	EditorObjectId id;

	auto operator<=>(const SnapshotStableReference&) const = default;
};

[[nodiscard]] bool snapshotReferencesMatch(
	const GltfDocument& document,
	const std::vector<SnapshotStableReference>& references) noexcept;
[[nodiscard]] bool sameMaterialSnapshotExceptProperties(
	const tinygltf::Material& left, const tinygltf::Material& right) noexcept;
[[nodiscard]] SceneMaterialParametersValue materialParameters(
	const tinygltf::Material& material);

[[nodiscard]] MutationResult mutateMaterialPropertiesOnly(
	GltfDocument& document, MaterialId material,
	const tinygltf::Material& snapshotRecord,
	const std::vector<SnapshotStableReference>& snapshotReferences,
	std::uint64_t snapshotBytes, const MutationGuard& guard,
	const EditorLimits& limits, EditorFaultInjector* faultInjector,
	std::uint64_t& serial)
{
	MutationResult result = baseResult(HistoryPolicy::undoable, document, guard, serial);
	if(result.error != MutationErrorCode::none) return result;
	if(material.authority() != document.documentAuthority())
	{
		result.error = MutationErrorCode::crossDocument;
		result.message = "material belongs to another document";
		return result;
	}
	if(document.revision() == std::numeric_limits<std::uint64_t>::max())
	{
		result.error = MutationErrorCode::revisionExhausted;
		result.message = "document revision is exhausted";
		return result;
	}
	const auto materialIndex = document.indexOf(material.untyped());
	if(!materialIndex || *materialIndex >= document.objectCount(EditorObjectKind::material))
	{
		result.error = MutationErrorCode::staleId;
		result.message = "material id is stale";
		return result;
	}
	const tinygltf::Material& current = detail::SceneCommandsAccess::model(document)
		.materials[*materialIndex];
	if(!snapshotReferencesMatch(document, snapshotReferences) ||
		!sameMaterialSnapshotExceptProperties(current, snapshotRecord) ||
		current.pbrMetallicRoughness.baseColorFactor.size() != 4u ||
		snapshotRecord.pbrMetallicRoughness.baseColorFactor.size() != 4u)
	{
		result.error = MutationErrorCode::staleTransaction;
		result.message = "material changed outside the supported material properties";
		return result;
	}
	const SceneMaterialParametersValue beforeParameters = materialParameters(current);
	const SceneMaterialParametersValue afterParameters = materialParameters(snapshotRecord);
	if(beforeParameters == afterParameters &&
		current.normalTexture.scale == snapshotRecord.normalTexture.scale &&
		current.occlusionTexture.strength == snapshotRecord.occlusionTexture.strength)
	{
		result.status = MutationStatus::domainNoop;
		result.error = MutationErrorCode::noOp;
		result.message = "operation has no document effect";
		return result;
	}
	if(snapshotBytes > limits.maxHistoryEntrySnapshotBytes)
	{
		result.error = MutationErrorCode::snapshotBudgetExceeded;
		result.message = "material snapshot exceeds the history budget";
		return result;
	}
	try
	{
		if(faultInjector != nullptr &&
			faultInjector->shouldFail(EditorFaultPoint::allocation))
		{
			result.error = MutationErrorCode::allocationFailed;
			result.message = "mutation allocation fault was injected before publication";
			return result;
		}
		Plan plan = makePlan(document);
		const SceneDigest baseDigest = document.documentDigest();
		if(!sceneDigestPresent(baseDigest))
		{
			result.error = MutationErrorCode::internalError;
			result.message = "document digest capture failed";
			return result;
		}
		if(faultInjector != nullptr &&
			faultInjector->shouldFail(EditorFaultPoint::preCandidatePublish))
		{
			result.error = MutationErrorCode::internalError;
			result.message = "mutation publication fault was injected";
			return result;
		}
		const std::uint64_t nextSerial = allocateMutationSerial();
		if(nextSerial == 0u)
		{
			result.error = MutationErrorCode::internalError;
			result.message = "mutation serial space is exhausted";
			return result;
		}
		struct PrePublishContext final
		{
			MutationResult* result;
			const Plan* before;
			GltfDocument* document;
			std::size_t materialIndex;
			const tinygltf::Material* record;
			SceneMaterialParametersValue beforeParameters;
			int beforeNormalIndex;
			int beforeNormalUv;
			double beforeNormalScale;
			int beforeOcclusionIndex;
			int beforeOcclusionUv;
			double beforeOcclusionStrength;
			EditorFaultInjector* faultInjector;
			EditorDocumentAuthority authority;
			std::uint64_t baseRevision;
			std::uint64_t targetRevision;
			std::uint64_t serial;
			SceneDigest baseDigest;
		} context{&result, &plan, &document, *materialIndex, &snapshotRecord,
			materialParameters(current), current.normalTexture.index,
			current.normalTexture.texCoord, current.normalTexture.scale,
			current.occlusionTexture.index, current.occlusionTexture.texCoord,
			current.occlusionTexture.strength, faultInjector,
			document.documentAuthority(), document.revision(),
			nextRevisionWithoutWrap(document.revision()), nextSerial, baseDigest};
		const auto prepareResult = [](const detail::MutationRegistryPlan& resolved,
			void* opaque)
		{
			auto& value = *static_cast<PrePublishContext*>(opaque);
			if(value.faultInjector != nullptr &&
				value.faultInjector->shouldFail(EditorFaultPoint::allocation))
				throw std::bad_alloc{};
			const SceneDigest targetDigest = value.document->documentDigest();
			if(!sceneDigestPresent(targetDigest))
				throw std::runtime_error("document digest capture failed");
			value.result->status = MutationStatus::applied;
			value.result->error = MutationErrorCode::none;
			value.result->message = "material properties updated";
			value.result->afterRevision = value.targetRevision;
			value.result->transactionToken = MutationToken{
				value.authority, value.targetRevision, value.serial};
			for(std::size_t kind = 1; kind < kindIndex(EditorObjectKind::count); ++kind)
			{
				addRemap(*value.result, value.before->idsByKind[kind],
					resolved.idsByKind[kind], static_cast<EditorObjectKind>(kind));
				addExactRemap(value.result->changes,
					value.before->idsByKind[kind], resolved.idsByKind[kind]);
			}
			value.result->changes.sourceMutation = SourceMutationFence{
				value.baseRevision, value.targetRevision,
				sceneMutationToken(value.result->transactionToken),
				value.baseDigest, targetDigest};
			const auto& afterMaterial = detail::SceneCommandsAccess::model(
				*value.document).materials[value.materialIndex];
			const StableSceneId target = stableSceneId(
				resolved.idsByKind[kindIndex(EditorObjectKind::material)]
					[value.materialIndex]);
			addUpdate(value.result->changes, target,
				SceneChangeClass::MaterialParameters,
				SceneField::MaterialParameters, value.serial,
				value.beforeParameters, materialParameters(afterMaterial));
			const auto& beforeModel = detail::SceneCommandsAccess::model(
				*value.document);
			addUpdate(value.result->changes, target,
				SceneChangeClass::MaterialBinding,
				SceneField::MaterialTextureBinding, value.serial,
				textureBinding(SceneTextureSemantic::Normal, value.beforeNormalIndex,
					value.beforeNormalUv, value.beforeNormalScale, beforeModel, *value.before),
				textureBinding(SceneTextureSemantic::Normal,
					value.record->normalTexture.index,
					value.record->normalTexture.texCoord,
					value.record->normalTexture.scale, beforeModel, *value.before));
			addUpdate(value.result->changes, target,
				SceneChangeClass::MaterialBinding,
				SceneField::MaterialTextureBinding, value.serial,
				textureBinding(SceneTextureSemantic::Occlusion,
					value.beforeOcclusionIndex, value.beforeOcclusionUv,
					value.beforeOcclusionStrength, beforeModel, *value.before),
				textureBinding(SceneTextureSemantic::Occlusion,
					value.record->occlusionTexture.index,
					value.record->occlusionTexture.texCoord,
					value.record->occlusionTexture.strength, beforeModel, *value.before));
			value.result->changedIds.push_back(editorObjectId(target));
		};
		detail::MutationCommitStatus commitStatus =
			detail::SceneCommandsAccess::commitMaterialProperties(
			 document, *materialIndex, snapshotRecord, plan, prepareResult, &context);
		if(commitStatus != detail::MutationCommitStatus::applied)
		{
			result.status = MutationStatus::rejected;
			result.afterRevision = result.beforeRevision;
			clearPublishedEvidence(result);
			result.transactionToken = MutationToken{
				document.documentAuthority(), document.revision(), serial};
			result.error = commitStatus == detail::MutationCommitStatus::allocationFailed
				? MutationErrorCode::allocationFailed
				: commitStatus == detail::MutationCommitStatus::revisionExhausted
					? MutationErrorCode::revisionExhausted
					: MutationErrorCode::internalError;
			result.message = "material Base Color commit failed before publication";
			return result;
		}
		serial = nextSerial;
		return result;
	}
	catch(const std::bad_alloc&)
	{
		result.error = MutationErrorCode::allocationFailed;
		result.message = "mutation allocation failed before commit";
		return result;
	}
	catch(...)
	{
		result.error = MutationErrorCode::internalError;
		result.message = "mutation failed before commit";
		return result;
	}
}

template <typename T>
void appendInvalid(std::vector<EditorObjectId>& ids, std::size_t count)
{
	ids.insert(ids.end(), count, EditorObjectId{});
}

void rebuildPrimitivePlan(Plan& plan,
	const std::vector<PrimitiveAddress>& oldAddresses,
	const std::vector<EditorObjectId>& oldIds,
	const tinygltf::Model& model)
{
	plan.primitiveAddresses.clear();
	plan.idsByKind[kindIndex(EditorObjectKind::primitive)].clear();
	for(std::size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex)
	{
		for(std::size_t primitiveIndex = 0;
			primitiveIndex < model.meshes[meshIndex].primitives.size(); ++primitiveIndex)
		{
			const PrimitiveAddress address{
				static_cast<std::uint32_t>(meshIndex),
				static_cast<std::uint32_t>(primitiveIndex)};
			plan.primitiveAddresses.push_back(address);
			const auto old = std::find(oldAddresses.begin(), oldAddresses.end(), address);
			plan.idsByKind[kindIndex(EditorObjectKind::primitive)].push_back(
				old != oldAddresses.end() ? oldIds[static_cast<std::size_t>(old - oldAddresses.begin())]
				                         : EditorObjectId{});
		}
	}
}

void setPrimitivePlanFromMeshIds(
	Plan& plan, const std::vector<std::vector<EditorObjectId>>& idsByMesh)
{
	plan.primitiveAddresses.clear();
	plan.idsByKind[kindIndex(EditorObjectKind::primitive)].clear();
	for(std::size_t meshIndex = 0; meshIndex < idsByMesh.size(); ++meshIndex)
	{
		for(std::size_t primitiveIndex = 0;
			primitiveIndex < idsByMesh[meshIndex].size(); ++primitiveIndex)
		{
			plan.primitiveAddresses.push_back(PrimitiveAddress{
				static_cast<std::uint32_t>(meshIndex),
				static_cast<std::uint32_t>(primitiveIndex)});
			plan.idsByKind[kindIndex(EditorObjectKind::primitive)].push_back(
				idsByMesh[meshIndex][primitiveIndex]);
		}
	}
}

[[nodiscard]] std::vector<std::vector<EditorObjectId>> primitiveIdsByMesh(
	const Plan& plan, std::size_t meshCount)
{
	std::vector<std::vector<EditorObjectId>> result(meshCount);
	const auto& ids = plan.idsByKind[kindIndex(EditorObjectKind::primitive)];
	for(std::size_t index = 0; index < plan.primitiveAddresses.size() && index < ids.size(); ++index)
	{
		const PrimitiveAddress address = plan.primitiveAddresses[index];
		if(address.meshIndex < result.size()) result[address.meshIndex].push_back(ids[index]);
	}
	return result;
}

[[nodiscard]] bool equivalentMeshGeometry(
	const tinygltf::Mesh& left, const tinygltf::Mesh& right)
{
	if(left.primitives.empty() || left.primitives.size() != right.primitives.size()
		|| left.weights != right.weights || left.extensions != right.extensions
		|| left.extras != right.extras
		|| left.extensions_json_string != right.extensions_json_string
		|| left.extras_json_string != right.extras_json_string) return false;
	for(std::size_t index = 0; index < left.primitives.size(); ++index)
	{
		const auto& a = left.primitives[index];
		const auto& b = right.primitives[index];
		if(a.attributes != b.attributes || a.indices != b.indices || a.mode != b.mode
			|| a.targets != b.targets || a.extensions != b.extensions
			|| a.extras != b.extras
			|| a.extensions_json_string != b.extensions_json_string
			|| a.extras_json_string != b.extras_json_string) return false;
	}
	return true;
}

[[nodiscard]] std::size_t meshUseCount(
	const tinygltf::Model& model, std::size_t meshIndex) noexcept
{
	return static_cast<std::size_t>(std::count_if(model.nodes.begin(), model.nodes.end(),
		[&](const tinygltf::Node& node)
		{
			return node.mesh == static_cast<int>(meshIndex);
		}));
}

[[nodiscard]] bool addSnapshotBytes(
	std::uint64_t& total, std::uint64_t value) noexcept
{
	std::uint64_t next = 0;
	if(!editorCheckedAdd(total, value, next)) return false;
	total = next;
	return true;
}

constexpr std::uint64_t kValueMapNodeCharge = 64u;
constexpr std::uint64_t kParameterMapNodeCharge = 96u;
constexpr std::uint64_t kPropertySnapshotControlCharge = 128u;
constexpr std::uint64_t kStructuralModelControlCharge = 256u;
constexpr std::uint64_t kStructuralDeltaControlCharge = 256u;

[[nodiscard]] bool addElementCharge(
	std::uint64_t& total,
	std::uint64_t count,
	std::uint64_t charge) noexcept
{
	std::uint64_t bytes = 0;
	return editorCheckedMultiply(count, charge, bytes)
		&& addSnapshotBytes(total, bytes);
}

template <typename T>
[[nodiscard]] bool addVectorCapacityBytes(
	std::uint64_t& total, const std::vector<T>& values) noexcept
{
	std::uint64_t bytes = 0;
	if(!editorCheckedMultiply(static_cast<std::uint64_t>(values.capacity()),
		static_cast<std::uint64_t>(sizeof(T)), bytes)) return false;
	return addSnapshotBytes(total, bytes);
}

[[nodiscard]] bool addStringCapacityBytes(
	std::uint64_t& total, const std::string& value) noexcept
{
	return addSnapshotBytes(total, static_cast<std::uint64_t>(value.capacity()) + 1u);
}

[[nodiscard]] bool addValueBytes(std::uint64_t& total, const tinygltf::Value& value) noexcept
{
	if(!addSnapshotBytes(total, sizeof(tinygltf::Value))) return false;
	if(value.IsString()) return addStringCapacityBytes(total, value.Get<std::string>());
	if(value.IsBinary())
		return addVectorCapacityBytes(total, value.Get<std::vector<unsigned char>>());
	if(value.IsArray())
	{
		const auto& values = value.Get<tinygltf::Value::Array>();
		if(!addVectorCapacityBytes(total, values)) return false;
		for(const auto& child : values) if(!addValueBytes(total, child)) return false;
	}
	else if(value.IsObject())
	{
		const auto& values = value.Get<tinygltf::Value::Object>();
		if(!addElementCharge(total, static_cast<std::uint64_t>(values.size()),
			kValueMapNodeCharge))
			return false;
		for(const auto& [name, child] : values)
		{
			if(!addStringCapacityBytes(total, name) || !addValueBytes(total, child)) return false;
		}
	}
	return true;
}

[[nodiscard]] bool addExtensionMapBytes(
	std::uint64_t& total, const tinygltf::ExtensionMap& values) noexcept
{
	if(!addElementCharge(total, static_cast<std::uint64_t>(values.size()),
		kValueMapNodeCharge))
		return false;
	for(const auto& [name, value] : values)
	{
		if(!addStringCapacityBytes(total, name) || !addValueBytes(total, value)) return false;
	}
	return true;
}

[[nodiscard]] bool addRecordValueBytes(
	std::uint64_t& total,
	const auto& record) noexcept
{
	return addStringCapacityBytes(total, record.extras_json_string)
		&& addStringCapacityBytes(total, record.extensions_json_string)
		&& addExtensionMapBytes(total, record.extensions)
		&& addValueBytes(total, record.extras);
}

[[nodiscard]] bool addTextureInfoBytes(
	std::uint64_t& total,
	const auto& texture) noexcept
{
	return addRecordValueBytes(total, texture);
}

[[nodiscard]] bool addParameterBytes(
	std::uint64_t& total,
	const tinygltf::Parameter& parameter) noexcept
{
	if(!addStringCapacityBytes(total, parameter.string_value)
		|| !addVectorCapacityBytes(total, parameter.number_array)
		|| !addElementCharge(total,
			static_cast<std::uint64_t>(parameter.json_double_value.size()),
			kValueMapNodeCharge))
		return false;
	for(const auto& [name, value] : parameter.json_double_value)
	{
		(void)value;
		if(!addStringCapacityBytes(total, name)) return false;
	}
	return true;
}

[[nodiscard]] bool addParameterMapBytes(
	std::uint64_t& total,
	const tinygltf::ParameterMap& parameters) noexcept
{
	if(!addElementCharge(total, static_cast<std::uint64_t>(parameters.size()),
		kParameterMapNodeCharge))
		return false;
	for(const auto& [name, parameter] : parameters)
		if(!addStringCapacityBytes(total, name)
			|| !addParameterBytes(total, parameter)) return false;
	return true;
}

[[nodiscard]] std::optional<std::uint64_t> materialRecordBytes(
	const tinygltf::Material& material) noexcept
{
	std::uint64_t total = sizeof(tinygltf::Material)
		+ kPropertySnapshotControlCharge;
	if(!addStringCapacityBytes(total, material.name)
		|| !addStringCapacityBytes(total, material.alphaMode)
		|| !addStringCapacityBytes(total, material.extras_json_string)
		|| !addStringCapacityBytes(total, material.extensions_json_string)
		|| !addVectorCapacityBytes(total, material.emissiveFactor)
		|| !addVectorCapacityBytes(total, material.lods)
		|| !addVectorCapacityBytes(total, material.pbrMetallicRoughness.baseColorFactor)
		|| !addExtensionMapBytes(total, material.extensions)
		|| !addRecordValueBytes(total, material.pbrMetallicRoughness)
		|| !addTextureInfoBytes(total, material.pbrMetallicRoughness.baseColorTexture)
		|| !addTextureInfoBytes(total, material.pbrMetallicRoughness.metallicRoughnessTexture)
		|| !addTextureInfoBytes(total, material.normalTexture)
		|| !addTextureInfoBytes(total, material.occlusionTexture)
		|| !addTextureInfoBytes(total, material.emissiveTexture)
		|| !addParameterMapBytes(total, material.values)
		|| !addParameterMapBytes(total, material.additionalValues)
		|| !addValueBytes(total, material.extras)) return std::nullopt;
	return total;
}

[[nodiscard]] std::optional<std::uint64_t> materialSnapshotBytes(
	const tinygltf::Material& material,
	const std::vector<SnapshotStableReference>& references) noexcept
{
	auto total = materialRecordBytes(material);
	if(!total
		|| !addSnapshotBytes(*total, sizeof(references))
		|| !addVectorCapacityBytes(*total, references)) return std::nullopt;
	return total;
}

[[nodiscard]] std::optional<std::uint64_t> lightRecordBytes(
	const tinygltf::Light& light) noexcept
{
	std::uint64_t total = sizeof(tinygltf::Light)
		+ kPropertySnapshotControlCharge;
	if(!addStringCapacityBytes(total, light.name)
		|| !addStringCapacityBytes(total, light.type)
		|| !addStringCapacityBytes(total, light.extras_json_string)
		|| !addStringCapacityBytes(total, light.extensions_json_string)
		|| !addVectorCapacityBytes(total, light.color)
		|| !addExtensionMapBytes(total, light.extensions)
		|| !addRecordValueBytes(total, light.spot)
		|| !addValueBytes(total, light.extras)) return std::nullopt;
	return total;
}

void eraseMesh(tinygltf::Model& model, Plan& plan, std::size_t meshIndex)
{
	auto idsByMesh = primitiveIdsByMesh(plan, model.meshes.size());
	model.meshes.erase(model.meshes.begin() + static_cast<std::ptrdiff_t>(meshIndex));
	idsByMesh.erase(idsByMesh.begin() + static_cast<std::ptrdiff_t>(meshIndex));
	plan.idsByKind[kindIndex(EditorObjectKind::mesh)].erase(
		plan.idsByKind[kindIndex(EditorObjectKind::mesh)].begin()
			+ static_cast<std::ptrdiff_t>(meshIndex));
	for(auto& node : model.nodes)
	{
		if(node.mesh == static_cast<int>(meshIndex)) node.mesh = -1;
		else if(node.mesh > static_cast<int>(meshIndex)) --node.mesh;
	}
	setPrimitivePlanFromMeshIds(plan, idsByMesh);
}

} // namespace

struct MaterialSnapshot::Impl final
{
	MaterialId id;
	EditorDocumentAuthority authority;
	tinygltf::Material record;
	std::vector<SnapshotStableReference> stableReferences;
	std::uint64_t bytes{0};
};

struct LightSnapshot::Impl final
{
	LightId id;
	EditorDocumentAuthority authority;
	tinygltf::Light record;
	std::uint64_t bytes{0};
};

namespace {

[[nodiscard]] bool appendSnapshotReference(
	const GltfDocument& document,
	EditorObjectKind kind,
	int sourceIndex,
	std::vector<SnapshotStableReference>& references,
	bool required)
{
	if(sourceIndex < 0) return true;
	if(static_cast<std::size_t>(sourceIndex) >= document.objectCount(kind))
		return !required;
	const EditorObjectId id = document.idAt(kind, static_cast<std::size_t>(sourceIndex));
	if(!id.valid() || id.kind != kind || id.authority != document.documentAuthority())
		return !required;
	const SnapshotStableReference value{kind, sourceIndex, id};
	if(std::find(references.begin(), references.end(), value) == references.end())
		references.push_back(value);
	return true;
}

[[nodiscard]] bool collectMaterialSnapshotReferences(
	const GltfDocument& document,
	const tinygltf::Material& material,
	std::vector<SnapshotStableReference>& references)
{
	references.clear();
	for(const int lod : material.lods)
		if(!appendSnapshotReference(document, EditorObjectKind::material,
			lod, references, true)) return false;
	for(const int texture : {
		material.pbrMetallicRoughness.baseColorTexture.index,
		material.pbrMetallicRoughness.metallicRoughnessTexture.index,
		material.normalTexture.index,
		material.occlusionTexture.index,
		material.emissiveTexture.index})
	{
		if(!appendSnapshotReference(document, EditorObjectKind::texture,
			texture, references, true)) return false;
	}
	const auto collectParameterMap = [&](const tinygltf::ParameterMap& parameters)
	{
		for(const auto& [name, parameter] : parameters)
		{
			(void)name;
			const auto found = parameter.json_double_value.find("index");
			if(found == parameter.json_double_value.end()
				|| !std::isfinite(found->second)
				|| std::trunc(found->second) != found->second
				|| found->second < 0.0
				|| found->second > static_cast<double>(std::numeric_limits<int>::max()))
				continue;
			(void)appendSnapshotReference(document, EditorObjectKind::texture,
				static_cast<int>(found->second), references, false);
		}
	};
	collectParameterMap(material.values);
	collectParameterMap(material.additionalValues);
	std::sort(references.begin(), references.end());
	references.erase(std::unique(references.begin(), references.end()), references.end());
	return true;
}

struct StructuralSideStorage final
{
	std::unique_ptr<tinygltf::Model> model;
	Plan plan;
	std::uint64_t documentRevision{0};
	std::vector<std::pair<EditorObjectId, std::uint64_t>> sceneHierarchyEpochs;
};

void stripStructuralPayloads(tinygltf::Model& model)
{
	tinygltf::Model retained;
	retained.scenes = std::move(model.scenes);
	retained.nodes = std::move(model.nodes);
	retained.skins = std::move(model.skins);
	retained.animations = std::move(model.animations);
	retained.lights = std::move(model.lights);
	retained.extensionsUsed = std::move(model.extensionsUsed);
	retained.extensionsRequired = std::move(model.extensionsRequired);
	model = std::move(retained);
}

[[nodiscard]] bool addRecordStrings(
	std::uint64_t& total, const auto& record) noexcept
{
	return addStringCapacityBytes(total, record.name)
		&& addStringCapacityBytes(total, record.extras_json_string)
		&& addStringCapacityBytes(total, record.extensions_json_string)
		&& addExtensionMapBytes(total, record.extensions)
		&& addValueBytes(total, record.extras);
}

[[nodiscard]] bool addAnimationChannelBytes(
	std::uint64_t& total, const tinygltf::AnimationChannel& channel) noexcept
{
	return addStringCapacityBytes(total, channel.target_path)
		&& addStringCapacityBytes(total, channel.extras_json_string)
		&& addStringCapacityBytes(total, channel.extensions_json_string)
		&& addStringCapacityBytes(total, channel.target_extras_json_string)
		&& addStringCapacityBytes(total, channel.target_extensions_json_string)
		&& addExtensionMapBytes(total, channel.extensions)
		&& addExtensionMapBytes(total, channel.target_extensions)
		&& addValueBytes(total, channel.extras)
		&& addValueBytes(total, channel.target_extras);
}

[[nodiscard]] bool addAnimationSamplerBytes(
	std::uint64_t& total, const tinygltf::AnimationSampler& sampler) noexcept
{
	return addStringCapacityBytes(total, sampler.interpolation)
		&& addStringCapacityBytes(total, sampler.extras_json_string)
		&& addStringCapacityBytes(total, sampler.extensions_json_string)
		&& addExtensionMapBytes(total, sampler.extensions)
		&& addValueBytes(total, sampler.extras);
}

[[nodiscard]] std::optional<std::uint64_t> structuralModelBytes(
	const tinygltf::Model& model) noexcept
{
	std::uint64_t total = sizeof(tinygltf::Model)
		+ kStructuralModelControlCharge;
	const auto addCollection = [&]<typename T>(const std::vector<T>& values) noexcept
	{
		return addVectorCapacityBytes(total, values);
	};
	if(!addCollection(model.scenes) || !addCollection(model.nodes)
		|| !addCollection(model.lights) || !addCollection(model.skins)
		|| !addCollection(model.animations)
		|| !addCollection(model.extensionsUsed)
		|| !addCollection(model.extensionsRequired))
		return std::nullopt;
	for(const auto& scene : model.scenes)
		if(!addRecordStrings(total, scene) || !addVectorCapacityBytes(total, scene.nodes)
			|| !addVectorCapacityBytes(total, scene.audioEmitters))
			return std::nullopt;
	for(const auto& node : model.nodes)
	{
		if(!addRecordStrings(total, node) || !addVectorCapacityBytes(total, node.children)
			|| !addVectorCapacityBytes(total, node.matrix)
			|| !addVectorCapacityBytes(total, node.translation)
			|| !addVectorCapacityBytes(total, node.rotation)
			|| !addVectorCapacityBytes(total, node.scale)
			|| !addVectorCapacityBytes(total, node.weights)
			|| !addVectorCapacityBytes(total, node.lods)) return std::nullopt;
	}
	for(const auto& skin : model.skins)
		if(!addRecordStrings(total, skin) || !addVectorCapacityBytes(total, skin.joints))
			return std::nullopt;
	for(const auto& animation : model.animations)
	{
		if(!addRecordStrings(total, animation)
			|| !addVectorCapacityBytes(total, animation.channels)
			|| !addVectorCapacityBytes(total, animation.samplers)) return std::nullopt;
		for(const auto& channel : animation.channels)
			if(!addAnimationChannelBytes(total, channel)) return std::nullopt;
		for(const auto& sampler : animation.samplers)
			if(!addAnimationSamplerBytes(total, sampler)) return std::nullopt;
	}
	for(const auto& light : model.lights)
	{
		const auto bytes = lightRecordBytes(light);
		if(!bytes || !addSnapshotBytes(total, *bytes)) return std::nullopt;
	}
	for(const std::string& name : model.extensionsUsed)
		if(!addStringCapacityBytes(total, name)) return std::nullopt;
	for(const std::string& name : model.extensionsRequired)
		if(!addStringCapacityBytes(total, name)) return std::nullopt;
	return total;
}

[[nodiscard]] std::optional<std::uint64_t> structuralPlanBytes(
	const Plan& plan) noexcept
{
	std::uint64_t total = 0;
	for(const auto& ids : plan.idsByKind)
		if(!addVectorCapacityBytes(total, ids)) return std::nullopt;
	if(!addVectorCapacityBytes(total, plan.primitiveAddresses)
		|| !addVectorCapacityBytes(total, plan.frozenSceneRootIndicesByScene)
		|| !addVectorCapacityBytes(total, plan.sceneRootStates)) return std::nullopt;
	for(const auto& frozen : plan.frozenSceneRootIndicesByScene)
		if(!addVectorCapacityBytes(total, frozen)) return std::nullopt;
	for(const auto& state : plan.sceneRootStates)
		if(!addVectorCapacityBytes(total, state.frozenRootIds)) return std::nullopt;
	return total;
}

using IdSet = std::unordered_set<EditorObjectId, EditorObjectIdHash>;

constexpr std::uint64_t kHashBucketCharge = sizeof(void*);
constexpr std::uint64_t kHashNodeOverhead = 32u;

[[nodiscard]] bool addIdSetBytes(
	std::uint64_t& total, const IdSet& values) noexcept
{
	return addElementCharge(total,
			static_cast<std::uint64_t>(values.bucket_count()), kHashBucketCharge)
		&& addElementCharge(total, static_cast<std::uint64_t>(values.size()),
			static_cast<std::uint64_t>(sizeof(EditorObjectId)) + kHashNodeOverhead);
}

[[nodiscard]] bool addStringSetBytes(
	std::uint64_t& total,
	const std::unordered_set<std::string>& values) noexcept
{
	if(!addElementCharge(total,
			static_cast<std::uint64_t>(values.bucket_count()), kHashBucketCharge)
		|| !addElementCharge(total, static_cast<std::uint64_t>(values.size()),
			static_cast<std::uint64_t>(sizeof(std::string)) + kHashNodeOverhead))
		return false;
	for(const std::string& value : values)
		if(!addStringCapacityBytes(total, value)) return false;
	return true;
}

[[nodiscard]] MutationErrorCode commitStatusError(
	detail::MutationCommitStatus status) noexcept
{
	switch(status)
	{
		case detail::MutationCommitStatus::allocationFailed:
			return MutationErrorCode::allocationFailed;
		case detail::MutationCommitStatus::generationExhausted:
			return MutationErrorCode::generationExhausted;
		case detail::MutationCommitStatus::revisionExhausted:
			return MutationErrorCode::revisionExhausted;
		case detail::MutationCommitStatus::sceneActivationEpochExhausted:
			return MutationErrorCode::sceneActivationEpochExhausted;
		case detail::MutationCommitStatus::hierarchyEpochExhausted:
			return MutationErrorCode::hierarchyEpochExhausted;
		case detail::MutationCommitStatus::externalStateSerialExhausted:
			return MutationErrorCode::externalStateSerialExhausted;
		default:
			return MutationErrorCode::internalError;
	}
}

[[nodiscard]] std::optional<std::size_t> stableIndex(
	const std::vector<EditorObjectId>& ids, EditorObjectId id) noexcept
{
	const auto found = std::find(ids.begin(), ids.end(), id);
	if(found == ids.end()) return std::nullopt;
	return static_cast<std::size_t>(found - ids.begin());
}

[[nodiscard]] std::optional<std::vector<EditorObjectId>> stableNodeList(
	const std::vector<int>& indices, const Plan& plan)
{
	std::vector<EditorObjectId> result;
	const auto& nodes = plan.idsByKind[kindIndex(EditorObjectKind::node)];
	result.reserve(indices.size());
	for(const int index : indices)
	{
		if(index < 0 || static_cast<std::size_t>(index) >= nodes.size()) return std::nullopt;
		result.push_back(nodes[static_cast<std::size_t>(index)]);
	}
	return result;
}

[[nodiscard]] std::optional<std::vector<int>> numericNodeList(
	const std::vector<EditorObjectId>& ids, const std::vector<EditorObjectId>& finalNodes)
{
	std::vector<int> result;
	result.reserve(ids.size());
	for(const EditorObjectId id : ids)
	{
		const auto index = stableIndex(finalNodes, id);
		if(!index || *index > static_cast<std::size_t>(std::numeric_limits<int>::max()))
			return std::nullopt;
		result.push_back(static_cast<int>(*index));
	}
	return result;
}

[[nodiscard]] IdSet symmetricOwned(
	const std::vector<EditorObjectId>& left,
	const std::vector<EditorObjectId>& right)
{
	IdSet result;
	for(const EditorObjectId id : left)
		if(std::find(right.begin(), right.end(), id) == right.end()) result.insert(id);
	for(const EditorObjectId id : right)
		if(std::find(left.begin(), left.end(), id) == left.end()) result.insert(id);
	return result;
}

void addMembershipDifference(
	const std::vector<EditorObjectId>& left,
	const std::vector<EditorObjectId>& right,
	IdSet& owned)
{
	for(const EditorObjectId id : left)
		if(std::find(right.begin(), right.end(), id) == right.end()) owned.insert(id);
	for(const EditorObjectId id : right)
		if(std::find(left.begin(), left.end(), id) == left.end()) owned.insert(id);
}

[[nodiscard]] std::optional<std::vector<EditorObjectId>> composeAnchored(
	const std::vector<EditorObjectId>& current,
	const std::vector<EditorObjectId>& source,
	const std::vector<EditorObjectId>& target,
	const IdSet& owned)
{
	const auto unique = [](const std::vector<EditorObjectId>& ids)
	{
		return std::unordered_set<EditorObjectId, EditorObjectIdHash>(
			ids.begin(), ids.end()).size() == ids.size();
	};
	if(!unique(current) || !unique(source) || !unique(target)) return std::nullopt;
	for(const EditorObjectId id : owned)
	{
		const bool expected = std::find(source.begin(), source.end(), id) != source.end();
		const bool actual = std::find(current.begin(), current.end(), id) != current.end();
		if(expected != actual) return std::nullopt;
	}
	std::vector<EditorObjectId> foreign;
	foreign.reserve(current.size());
	for(const EditorObjectId id : current)
		if(!owned.contains(id)) foreign.push_back(id);

	struct ResolvedAnchor final
	{
		std::size_t capturedPosition{0};
		std::size_t foreignPosition{0};
		EditorObjectId id;
	};
	struct OwnedRun final
	{
		std::size_t begin{0};
		std::size_t end{0};
		std::size_t boundary{0};
	};
	std::vector<ResolvedAnchor> anchors;
	std::vector<OwnedRun> runs;
	for(std::size_t begin = 0; begin < target.size();)
	{
		if(!owned.contains(target[begin])) { ++begin; continue; }
		std::size_t end = begin + 1u;
		while(end < target.size() && owned.contains(target[end])) ++end;
		std::optional<std::pair<std::size_t, std::size_t>> predecessor;
		for(std::size_t index = begin; index > 0; --index)
		{
			const EditorObjectId candidate = target[index - 1u];
			const auto found = std::find(foreign.begin(), foreign.end(), candidate);
			if(!owned.contains(candidate) && found != foreign.end())
			{
				predecessor = std::pair{
					index - 1u,
					static_cast<std::size_t>(found - foreign.begin())};
				break;
			}
		}
		std::optional<std::pair<std::size_t, std::size_t>> successor;
		for(std::size_t index = end; index < target.size(); ++index)
		{
			const EditorObjectId candidate = target[index];
			const auto found = std::find(foreign.begin(), foreign.end(), candidate);
			if(!owned.contains(candidate) && found != foreign.end())
			{
				successor = std::pair{
					index,
					static_cast<std::size_t>(found - foreign.begin())};
				break;
			}
		}
		if(predecessor && successor
			&& predecessor->second >= successor->second) return std::nullopt;
		const std::size_t boundary = predecessor
			? predecessor->second + 1u
			: successor ? successor->second : 0u;
		if(!runs.empty() && boundary < runs.back().boundary) return std::nullopt;
		if(predecessor)
			anchors.push_back(ResolvedAnchor{
				predecessor->first, predecessor->second, target[predecessor->first]});
		if(successor)
			anchors.push_back(ResolvedAnchor{
				successor->first, successor->second, target[successor->first]});
		runs.push_back(OwnedRun{begin, end, boundary});
		begin = end;
	}
	std::stable_sort(anchors.begin(), anchors.end(), [](const auto& left, const auto& right)
	{
		return left.capturedPosition < right.capturedPosition;
	});
	for(std::size_t index = 1; index < anchors.size(); ++index)
	{
		if(anchors[index - 1u].capturedPosition < anchors[index].capturedPosition
			&& anchors[index - 1u].foreignPosition >= anchors[index].foreignPosition)
			return std::nullopt;
		if(anchors[index - 1u].capturedPosition == anchors[index].capturedPosition
			&& anchors[index - 1u].id != anchors[index].id) return std::nullopt;
	}

	std::vector<std::vector<EditorObjectId>> buckets(foreign.size() + 1u);
	for(const OwnedRun& run : runs)
		buckets[run.boundary].insert(buckets[run.boundary].end(),
			target.begin() + static_cast<std::ptrdiff_t>(run.begin),
			target.begin() + static_cast<std::ptrdiff_t>(run.end));
	std::vector<EditorObjectId> result;
	result.reserve(foreign.size() + target.size());
	for(std::size_t boundary = 0; boundary <= foreign.size(); ++boundary)
	{
		result.insert(result.end(), buckets[boundary].begin(), buckets[boundary].end());
		if(boundary < foreign.size()) result.push_back(foreign[boundary]);
	}
	if(!unique(result)) return std::nullopt;
	return result;
}

[[nodiscard]] bool sameNodeRecordWithoutMembership(const tinygltf::Node& left,
	const tinygltf::Node& right)
{
	tinygltf::Node a = left;
	tinygltf::Node b = right;
	a.children.clear();
	b.children.clear();
	return a == b;
}

[[maybe_unused]] [[nodiscard]] bool sameSceneRecordWithoutMembership(const tinygltf::Scene& left,
	const tinygltf::Scene& right)
{
	tinygltf::Scene a = left;
	tinygltf::Scene b = right;
	a.nodes.clear();
	b.nodes.clear();
	return a == b;
}

template <typename Record>
[[nodiscard]] bool chooseStructuralRecord(EditorObjectId id,
	const std::vector<EditorObjectId>& currentIds,
	const std::vector<Record>& currentRecords,
	const std::vector<EditorObjectId>& sourceIds,
	const std::vector<Record>& sourceRecords,
	const std::vector<EditorObjectId>& targetIds,
	const std::vector<Record>& targetRecords,
	const IdSet& collectionOwned,
	const IdSet& recordOwned,
	Record& output,
	bool& fromTarget)
{
	const auto current = stableIndex(currentIds, id);
	const auto target = stableIndex(targetIds, id);
	if(target && (collectionOwned.contains(id) || recordOwned.contains(id)))
	{
		output = targetRecords[*target];
		fromTarget = true;
		return true;
	}
	if(current)
	{
		output = currentRecords[*current];
		fromTarget = false;
		return true;
	}
	if(target)
	{
		output = targetRecords[*target];
		fromTarget = true;
		return true;
	}
	(void)sourceIds;
	(void)sourceRecords;
	return false;
}

[[nodiscard]] bool remapStableReference(
	int& reference,
	EditorObjectKind kind,
	const Plan& origin,
	const Plan& destination,
	bool optional = true) noexcept
{
	if(reference < 0) return optional;
	const auto& originIds = origin.idsByKind[kindIndex(kind)];
	if(static_cast<std::size_t>(reference) >= originIds.size()) return false;
	const auto mapped = stableIndex(
		destination.idsByKind[kindIndex(kind)], originIds[static_cast<std::size_t>(reference)]);
	if(!mapped || *mapped > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
	reference = static_cast<int>(*mapped);
	return true;
}

[[nodiscard]] bool normalizeNodeRecord(
	tinygltf::Node& node, const Plan& origin, const Plan& destination)
{
	if(!remapStableReference(node.mesh, EditorObjectKind::mesh, origin, destination)
		|| !remapStableReference(node.camera, EditorObjectKind::camera, origin, destination)
		|| !remapStableReference(node.skin, EditorObjectKind::skin, origin, destination)
		|| !remapStableReference(node.light, EditorObjectKind::light, origin, destination))
		return false;
	for(int& lod : node.lods)
		if(!remapStableReference(lod, EditorObjectKind::node, origin, destination, false)) return false;
	auto extension = node.extensions.find("KHR_lights_punctual");
	if(extension != node.extensions.end() && extension->second.IsObject()
		&& extension->second.Has("light"))
	{
		auto& value = extension->second.Get<tinygltf::Value::Object>()["light"];
		if(!value.IsInt()) return false;
		int light = value.GetNumberAsInt();
		if(!remapStableReference(light, EditorObjectKind::light, origin, destination, false))
			return false;
		value = tinygltf::Value(light);
	}
	return true;
}

[[nodiscard]] bool normalizeSkinRecord(
	tinygltf::Skin& skin, const Plan& origin, const Plan& destination)
{
	for(int& joint : skin.joints)
		if(!remapStableReference(joint, EditorObjectKind::node, origin, destination, false))
			return false;
	return remapStableReference(
		skin.skeleton, EditorObjectKind::node, origin, destination);
}

[[nodiscard]] bool normalizeAnimationRecord(
	tinygltf::Animation& animation, const Plan& origin, const Plan& destination)
{
	for(auto& channel : animation.channels)
		if(!remapStableReference(
			channel.target_node, EditorObjectKind::node, origin, destination)) return false;
	return true;
}

[[nodiscard]] std::optional<std::vector<std::string>> composeExtensionNames(
	const std::vector<std::string>& current,
	const std::vector<std::string>& source,
	const std::vector<std::string>& target,
	const std::unordered_set<std::string>& owned,
	std::string_view externallyRequiredName = {})
{
	for(const std::string& name : owned)
	{
		const bool expected = std::find(source.begin(), source.end(), name) != source.end();
		const bool actual = std::find(current.begin(), current.end(), name) != current.end();
		if(expected != actual
			&& !(actual && name == externallyRequiredName)) return std::nullopt;
	}
	std::vector<std::string> result;
	result.reserve(current.size() + owned.size());
	for(const std::string& name : current)
		if(!owned.contains(name)) result.push_back(name);
	for(const std::string& name : target)
		if(owned.contains(name)) result.push_back(name);
	if(std::unordered_set<std::string>(result.begin(), result.end()).size() != result.size())
		return std::nullopt;
	return result;
}

class CanonicalStructuralWriter final
{
public:
	void reserve(std::size_t bytes) { m_bytes.reserve(bytes); }

	void u8(std::uint8_t value) { m_bytes.push_back(value); }

	void u32(std::uint32_t value)
	{
		for(unsigned shift = 0; shift < 32u; shift += 8u)
			m_bytes.push_back(static_cast<std::uint8_t>(value >> shift));
	}

	void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }

	void u64(std::uint64_t value)
	{
		for(unsigned shift = 0; shift < 64u; shift += 8u)
			m_bytes.push_back(static_cast<std::uint8_t>(value >> shift));
	}

	void f64(double value) { u64(detail::bitwiseCast<std::uint64_t>(value)); }
	void boolean(bool value) { u8(value ? 1u : 0u); }

	void raw(std::string_view value) { m_bytes.insert(m_bytes.end(), value.begin(), value.end()); }

	void string(std::string_view value)
	{
		u64(static_cast<std::uint64_t>(value.size()));
		raw(value);
	}

	void binary(const std::vector<unsigned char>& value)
	{
		u64(static_cast<std::uint64_t>(value.size()));
		m_bytes.insert(m_bytes.end(), value.begin(), value.end());
	}

	[[nodiscard]] std::size_t beginSection(std::uint32_t tag)
	{
		u32(tag);
		const std::size_t lengthOffset = m_bytes.size();
		u64(0u);
		return lengthOffset;
	}

	void endSection(std::size_t lengthOffset) noexcept
	{
		const std::uint64_t length =
			static_cast<std::uint64_t>(m_bytes.size() - lengthOffset - sizeof(std::uint64_t));
		for(unsigned shift = 0; shift < 64u; shift += 8u)
			m_bytes[lengthOffset + shift / 8u] = static_cast<std::uint8_t>(length >> shift);
	}

	[[nodiscard]] std::vector<std::uint8_t> take() noexcept { return std::move(m_bytes); }

private:
	std::vector<std::uint8_t> m_bytes;
};

static_assert(sizeof(double) == sizeof(std::uint64_t));
static_assert(std::numeric_limits<double>::is_iec559);

[[nodiscard]] bool sameSnapshotDouble(double left, double right) noexcept
{
	return detail::bitwiseCast<std::uint64_t>(left)
		== detail::bitwiseCast<std::uint64_t>(right);
}

[[nodiscard]] bool sameSnapshotDoubles(
	const std::vector<double>& left, const std::vector<double>& right) noexcept
{
	if(left.size() != right.size()) return false;
	for(std::size_t index = 0; index < left.size(); ++index)
		if(!sameSnapshotDouble(left[index], right[index])) return false;
	return true;
}

[[nodiscard]] bool sameSnapshotValue(
	const tinygltf::Value& left, const tinygltf::Value& right) noexcept
{
	if(left.Type() != right.Type()) return false;
	switch(left.Type())
	{
		case tinygltf::NULL_TYPE:
			return true;
		case tinygltf::BOOL_TYPE:
			return left.Get<bool>() == right.Get<bool>();
		case tinygltf::INT_TYPE:
			return left.Get<int>() == right.Get<int>();
		case tinygltf::REAL_TYPE:
			return sameSnapshotDouble(left.Get<double>(), right.Get<double>());
		case tinygltf::STRING_TYPE:
			return left.Get<std::string>() == right.Get<std::string>();
		case tinygltf::BINARY_TYPE:
			return left.Get<std::vector<unsigned char>>()
				== right.Get<std::vector<unsigned char>>();
		case tinygltf::ARRAY_TYPE:
		{
			const auto& leftValues = left.Get<tinygltf::Value::Array>();
			const auto& rightValues = right.Get<tinygltf::Value::Array>();
			if(leftValues.size() != rightValues.size()) return false;
			for(std::size_t index = 0; index < leftValues.size(); ++index)
				if(!sameSnapshotValue(leftValues[index], rightValues[index])) return false;
			return true;
		}
		case tinygltf::OBJECT_TYPE:
		{
			const auto& leftValues = left.Get<tinygltf::Value::Object>();
			const auto& rightValues = right.Get<tinygltf::Value::Object>();
			if(leftValues.size() != rightValues.size()) return false;
			auto leftValue = leftValues.begin();
			auto rightValue = rightValues.begin();
			for(; leftValue != leftValues.end(); ++leftValue, ++rightValue)
			{
				if(leftValue->first != rightValue->first
					|| !sameSnapshotValue(leftValue->second, rightValue->second))
					return false;
			}
			return true;
		}
		default:
			return false;
	}
}

[[nodiscard]] bool sameSnapshotExtensions(
	const tinygltf::ExtensionMap& left, const tinygltf::ExtensionMap& right) noexcept
{
	if(left.size() != right.size()) return false;
	auto leftValue = left.begin();
	auto rightValue = right.begin();
	for(; leftValue != left.end(); ++leftValue, ++rightValue)
	{
		if(leftValue->first != rightValue->first
			|| !sameSnapshotValue(leftValue->second, rightValue->second))
			return false;
	}
	return true;
}

template <typename Record>
[[nodiscard]] bool sameSnapshotRecordTail(const Record& left, const Record& right) noexcept
{
	return sameSnapshotExtensions(left.extensions, right.extensions)
		&& sameSnapshotValue(left.extras, right.extras)
		&& left.extensions_json_string == right.extensions_json_string
		&& left.extras_json_string == right.extras_json_string;
}

[[nodiscard]] bool sameSnapshotTextureInfo(
	const tinygltf::TextureInfo& left, const tinygltf::TextureInfo& right) noexcept
{
	return left.index == right.index && left.texCoord == right.texCoord
		&& sameSnapshotRecordTail(left, right);
}

[[nodiscard]] bool sameSnapshotNormalTextureInfo(
	const tinygltf::NormalTextureInfo& left, const tinygltf::NormalTextureInfo& right) noexcept
{
	return left.index == right.index && left.texCoord == right.texCoord
		&& sameSnapshotDouble(left.scale, right.scale) && sameSnapshotRecordTail(left, right);
}

[[nodiscard]] bool sameSnapshotOcclusionTextureInfo(const tinygltf::OcclusionTextureInfo& left,
	const tinygltf::OcclusionTextureInfo& right) noexcept
{
	return left.index == right.index && left.texCoord == right.texCoord
		&& sameSnapshotDouble(left.strength, right.strength) && sameSnapshotRecordTail(left, right);
}

[[nodiscard]] bool sameSnapshotParameter(
	const tinygltf::Parameter& left, const tinygltf::Parameter& right) noexcept
{
	if(left.bool_value != right.bool_value || left.has_number_value != right.has_number_value
		|| left.string_value != right.string_value
		|| !sameSnapshotDoubles(left.number_array, right.number_array)
		|| left.json_double_value.size() != right.json_double_value.size()
		|| !sameSnapshotDouble(left.number_value, right.number_value))
		return false;
	auto leftValue = left.json_double_value.begin();
	auto rightValue = right.json_double_value.begin();
	for(; leftValue != left.json_double_value.end(); ++leftValue, ++rightValue)
	{
		if(leftValue->first != rightValue->first
			|| !sameSnapshotDouble(leftValue->second, rightValue->second))
			return false;
	}
	return true;
}

[[nodiscard]] bool sameSnapshotParameterMap(
	const tinygltf::ParameterMap& left, const tinygltf::ParameterMap& right) noexcept
{
	if(left.size() != right.size()) return false;
	auto leftValue = left.begin();
	auto rightValue = right.begin();
	for(; leftValue != left.end(); ++leftValue, ++rightValue)
	{
		if(leftValue->first != rightValue->first
			|| !sameSnapshotParameter(leftValue->second, rightValue->second))
			return false;
	}
	return true;
}

[[nodiscard]] bool sameSnapshotPbrMetallicRoughness(const tinygltf::PbrMetallicRoughness& left,
	const tinygltf::PbrMetallicRoughness& right) noexcept
{
	return sameSnapshotDoubles(left.baseColorFactor, right.baseColorFactor)
		&& sameSnapshotTextureInfo(left.baseColorTexture, right.baseColorTexture)
		&& sameSnapshotDouble(left.metallicFactor, right.metallicFactor)
		&& sameSnapshotDouble(left.roughnessFactor, right.roughnessFactor)
		&& sameSnapshotTextureInfo(left.metallicRoughnessTexture, right.metallicRoughnessTexture)
		&& sameSnapshotRecordTail(left, right);
}

[[nodiscard]] bool sameMaterialSnapshotRecord(
	const tinygltf::Material& left, const tinygltf::Material& right) noexcept
{
	return left.name == right.name && sameSnapshotDoubles(left.emissiveFactor, right.emissiveFactor)
		&& left.alphaMode == right.alphaMode
		&& sameSnapshotDouble(left.alphaCutoff, right.alphaCutoff)
		&& left.doubleSided == right.doubleSided && left.lods == right.lods
		&& sameSnapshotPbrMetallicRoughness(left.pbrMetallicRoughness, right.pbrMetallicRoughness)
		&& sameSnapshotNormalTextureInfo(left.normalTexture, right.normalTexture)
		&& sameSnapshotOcclusionTextureInfo(left.occlusionTexture, right.occlusionTexture)
		&& sameSnapshotTextureInfo(left.emissiveTexture, right.emissiveTexture)
		&& sameSnapshotParameterMap(left.values, right.values)
		&& sameSnapshotParameterMap(left.additionalValues, right.additionalValues)
		&& sameSnapshotRecordTail(left, right);
}

[[nodiscard]] bool sameMaterialSnapshotExceptProperties(
	const tinygltf::Material& left, const tinygltf::Material& right) noexcept
{
	return left.name == right.name && left.lods == right.lods &&
		left.alphaMode == right.alphaMode &&
		left.doubleSided == right.doubleSided &&
		sameSnapshotTextureInfo(left.pbrMetallicRoughness.baseColorTexture,
			right.pbrMetallicRoughness.baseColorTexture) &&
		sameSnapshotTextureInfo(
			left.pbrMetallicRoughness.metallicRoughnessTexture,
			right.pbrMetallicRoughness.metallicRoughnessTexture) &&
		sameSnapshotRecordTail(left.pbrMetallicRoughness,
			right.pbrMetallicRoughness) &&
		left.normalTexture.index == right.normalTexture.index &&
		left.normalTexture.texCoord == right.normalTexture.texCoord &&
		sameSnapshotRecordTail(left.normalTexture, right.normalTexture) &&
		left.occlusionTexture.index == right.occlusionTexture.index &&
		left.occlusionTexture.texCoord == right.occlusionTexture.texCoord &&
		sameSnapshotRecordTail(left.occlusionTexture, right.occlusionTexture) &&
		sameSnapshotTextureInfo(left.emissiveTexture, right.emissiveTexture) &&
		sameSnapshotParameterMap(left.values, right.values) &&
		sameSnapshotParameterMap(left.additionalValues, right.additionalValues) &&
		sameSnapshotRecordTail(left, right);
}

[[nodiscard]] bool sameLightSnapshotRecord(
	const tinygltf::Light& left, const tinygltf::Light& right) noexcept
{
	return left.name == right.name && sameSnapshotDoubles(left.color, right.color)
		&& sameSnapshotDouble(left.intensity, right.intensity) && left.type == right.type
		&& sameSnapshotDouble(left.range, right.range)
		&& sameSnapshotDouble(left.spot.innerConeAngle, right.spot.innerConeAngle)
		&& sameSnapshotDouble(left.spot.outerConeAngle, right.spot.outerConeAngle)
		&& sameSnapshotRecordTail(left.spot, right.spot) && sameSnapshotRecordTail(left, right);
}

[[nodiscard]] bool snapshotReferencesMatch(
	const GltfDocument& document, const std::vector<SnapshotStableReference>& references) noexcept
{
	for(const SnapshotStableReference& reference : references)
	{
		if(reference.sourceIndex < 0 || reference.kind <= EditorObjectKind::invalid
			|| reference.kind >= EditorObjectKind::count
			|| document.idAt(reference.kind, static_cast<std::size_t>(reference.sourceIndex))
				!= reference.id)
			return false;
	}
	return true;
}

constexpr std::uint32_t canonicalTag(char a, char b, char c, char d) noexcept
{
	return static_cast<std::uint32_t>(static_cast<unsigned char>(a))
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8u)
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16u)
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24u);
}

void writeCanonicalId(CanonicalStructuralWriter& writer, EditorObjectId id)
{
	writer.u8(static_cast<std::uint8_t>(id.kind));
	writer.u32(id.slot);
	writer.u32(id.generation);
	writer.u64(id.authority.value);
}

void writeCanonicalIds(CanonicalStructuralWriter& writer, const std::vector<EditorObjectId>& ids)
{
	writer.u64(static_cast<std::uint64_t>(ids.size()));
	for(const EditorObjectId id : ids) writeCanonicalId(writer, id);
}

void writeCanonicalReference(CanonicalStructuralWriter& writer,
	int reference,
	EditorObjectKind kind,
	const Plan& plan)
{
	if(reference < 0)
	{
		writer.u8(0u);
		return;
	}
	const auto& ids = plan.idsByKind[kindIndex(kind)];
	if(static_cast<std::size_t>(reference) < ids.size()
		&& ids[static_cast<std::size_t>(reference)].valid())
	{
		writer.u8(1u);
		writeCanonicalId(writer, ids[static_cast<std::size_t>(reference)]);
		return;
	}
	writer.u8(2u);
	writer.i32(reference);
}

void writeCanonicalReferences(CanonicalStructuralWriter& writer,
	const std::vector<int>& references,
	EditorObjectKind kind,
	const Plan& plan)
{
	writer.u64(static_cast<std::uint64_t>(references.size()));
	for(const int reference : references) writeCanonicalReference(writer, reference, kind, plan);
}

void writeCanonicalDoubles(CanonicalStructuralWriter& writer, const std::vector<double>& values)
{
	writer.u64(static_cast<std::uint64_t>(values.size()));
	for(const double value : values) writer.f64(value);
}

template <std::size_t Size>
void writeCanonicalDoubles(CanonicalStructuralWriter& writer,
	const std::array<double, Size>& values)
{
	writer.u64(static_cast<std::uint64_t>(Size));
	for(const double value : values) writer.f64(value);
}

void writeCanonicalValue(CanonicalStructuralWriter& writer, const tinygltf::Value& value)
{
	switch(value.Type())
	{
		case tinygltf::NULL_TYPE:
			writer.u8(0u);
			break;
		case tinygltf::BOOL_TYPE:
			writer.u8(1u);
			writer.boolean(value.Get<bool>());
			break;
		case tinygltf::INT_TYPE:
			writer.u8(2u);
			writer.i32(value.Get<int>());
			break;
		case tinygltf::REAL_TYPE:
			writer.u8(3u);
			writer.f64(value.Get<double>());
			break;
		case tinygltf::STRING_TYPE:
			writer.u8(4u);
			writer.string(value.Get<std::string>());
			break;
		case tinygltf::BINARY_TYPE:
			writer.u8(5u);
			writer.binary(value.Get<std::vector<unsigned char>>());
			break;
		case tinygltf::ARRAY_TYPE:
		{
			writer.u8(6u);
			const auto& values = value.Get<tinygltf::Value::Array>();
			writer.u64(static_cast<std::uint64_t>(values.size()));
			for(const auto& child : values) writeCanonicalValue(writer, child);
			break;
		}
		case tinygltf::OBJECT_TYPE:
		{
			writer.u8(7u);
			const auto& values = value.Get<tinygltf::Value::Object>();
			writer.u64(static_cast<std::uint64_t>(values.size()));
			for(const auto& [name, child] : values)
			{
				writer.string(name);
				writeCanonicalValue(writer, child);
			}
			break;
		}
		default:
			writer.u8(0xffu);
			break;
	}
}

void writeCanonicalExtensions(CanonicalStructuralWriter& writer,
	const tinygltf::ExtensionMap& extensions)
{
	writer.u64(static_cast<std::uint64_t>(extensions.size()));
	for(const auto& [name, value] : extensions)
	{
		writer.string(name);
		writeCanonicalValue(writer, value);
	}
}

void writeCanonicalRecordTail(CanonicalStructuralWriter& writer, const auto& record)
{
	writeCanonicalExtensions(writer, record.extensions);
	writeCanonicalValue(writer, record.extras);
	writer.string(record.extensions_json_string);
	writer.string(record.extras_json_string);
}

void writeCanonicalScene(CanonicalStructuralWriter& writer,
	const tinygltf::Scene& scene,
	const Plan& plan)
{
	writer.string(scene.name);
	writeCanonicalReferences(writer, scene.nodes, EditorObjectKind::node, plan);
	writer.u64(static_cast<std::uint64_t>(scene.audioEmitters.size()));
	for(const int emitter : scene.audioEmitters) writer.i32(emitter);
	writeCanonicalRecordTail(writer, scene);
}

void writeCanonicalNode(CanonicalStructuralWriter& writer,
	const tinygltf::Node& node,
	const Plan& plan)
{
	writer.string(node.name);
	writeCanonicalReference(writer, node.camera, EditorObjectKind::camera, plan);
	writeCanonicalReference(writer, node.skin, EditorObjectKind::skin, plan);
	writeCanonicalReference(writer, node.mesh, EditorObjectKind::mesh, plan);
	writeCanonicalReference(writer, node.light, EditorObjectKind::light, plan);
	writer.i32(node.emitter);
	writeCanonicalReferences(writer, node.lods, EditorObjectKind::node, plan);
	writeCanonicalReferences(writer, node.children, EditorObjectKind::node, plan);
	writeCanonicalDoubles(writer, node.rotation);
	writeCanonicalDoubles(writer, node.scale);
	writeCanonicalDoubles(writer, node.translation);
	writeCanonicalDoubles(writer, node.matrix);
	writeCanonicalDoubles(writer, node.weights);
	writeCanonicalRecordTail(writer, node);
}

void writeCanonicalSkin(CanonicalStructuralWriter& writer,
	const tinygltf::Skin& skin,
	const Plan& plan)
{
	writer.string(skin.name);
	writeCanonicalReference(writer, skin.inverseBindMatrices, EditorObjectKind::accessor, plan);
	writeCanonicalReference(writer, skin.skeleton, EditorObjectKind::node, plan);
	writeCanonicalReferences(writer, skin.joints, EditorObjectKind::node, plan);
	writeCanonicalRecordTail(writer, skin);
}

void writeCanonicalAnimation(CanonicalStructuralWriter& writer,
	const tinygltf::Animation& animation,
	const Plan& plan)
{
	writer.string(animation.name);
	writer.u64(static_cast<std::uint64_t>(animation.channels.size()));
	for(const auto& channel : animation.channels)
	{
		writer.i32(channel.sampler);
		writeCanonicalReference(writer, channel.target_node, EditorObjectKind::node, plan);
		writer.string(channel.target_path);
		writeCanonicalExtensions(writer, channel.extensions);
		writeCanonicalValue(writer, channel.extras);
		writeCanonicalExtensions(writer, channel.target_extensions);
		writeCanonicalValue(writer, channel.target_extras);
		writer.string(channel.extensions_json_string);
		writer.string(channel.extras_json_string);
		writer.string(channel.target_extensions_json_string);
		writer.string(channel.target_extras_json_string);
	}
	writer.u64(static_cast<std::uint64_t>(animation.samplers.size()));
	for(const auto& sampler : animation.samplers)
	{
		writeCanonicalReference(writer, sampler.input, EditorObjectKind::accessor, plan);
		writeCanonicalReference(writer, sampler.output, EditorObjectKind::accessor, plan);
		writer.string(sampler.interpolation);
		writeCanonicalRecordTail(writer, sampler);
	}
	writeCanonicalRecordTail(writer, animation);
}

void writeCanonicalLight(CanonicalStructuralWriter& writer, const tinygltf::Light& light)
{
	writer.string(light.name);
	writeCanonicalDoubles(writer, light.color);
	writer.f64(light.intensity);
	writer.string(light.type);
	writer.f64(light.range);
	writer.f64(light.spot.innerConeAngle);
	writer.f64(light.spot.outerConeAngle);
	writeCanonicalRecordTail(writer, light.spot);
	writeCanonicalRecordTail(writer, light);
}

void writeCanonicalTextureInfo(CanonicalStructuralWriter& writer,
	const tinygltf::TextureInfo& texture)
{
	writer.i32(texture.index);
	writer.i32(texture.texCoord);
	writeCanonicalRecordTail(writer, texture);
}

void writeCanonicalNormalTextureInfo(CanonicalStructuralWriter& writer,
	const tinygltf::NormalTextureInfo& texture)
{
	writer.i32(texture.index);
	writer.i32(texture.texCoord);
	writer.f64(texture.scale);
	writeCanonicalRecordTail(writer, texture);
}

void writeCanonicalOcclusionTextureInfo(CanonicalStructuralWriter& writer,
	const tinygltf::OcclusionTextureInfo& texture)
{
	writer.i32(texture.index);
	writer.i32(texture.texCoord);
	writer.f64(texture.strength);
	writeCanonicalRecordTail(writer, texture);
}

void writeCanonicalParameter(CanonicalStructuralWriter& writer,
	const tinygltf::Parameter& parameter)
{
	writer.boolean(parameter.bool_value);
	writer.boolean(parameter.has_number_value);
	writer.string(parameter.string_value);
	writeCanonicalDoubles(writer, parameter.number_array);
	writer.u64(static_cast<std::uint64_t>(parameter.json_double_value.size()));
	for(const auto& [name, value] : parameter.json_double_value)
	{
		writer.string(name);
		writer.f64(value);
	}
	writer.f64(parameter.number_value);
}

void writeCanonicalParameterMap(CanonicalStructuralWriter& writer,
	const tinygltf::ParameterMap& parameters)
{
	writer.u64(static_cast<std::uint64_t>(parameters.size()));
	for(const auto& [name, parameter] : parameters)
	{
		writer.string(name);
		writeCanonicalParameter(writer, parameter);
	}
}

void writeCanonicalPbrMetallicRoughness(CanonicalStructuralWriter& writer,
	const tinygltf::PbrMetallicRoughness& pbr)
{
	writeCanonicalDoubles(writer, pbr.baseColorFactor);
	writeCanonicalTextureInfo(writer, pbr.baseColorTexture);
	writer.f64(pbr.metallicFactor);
	writer.f64(pbr.roughnessFactor);
	writeCanonicalTextureInfo(writer, pbr.metallicRoughnessTexture);
	writeCanonicalRecordTail(writer, pbr);
}

void writeCanonicalMaterial(CanonicalStructuralWriter& writer,
	const tinygltf::Material& material)
{
	writer.string(material.name);
	writeCanonicalDoubles(writer, material.emissiveFactor);
	writer.string(material.alphaMode);
	writer.f64(material.alphaCutoff);
	writer.boolean(material.doubleSided);
	writer.u64(static_cast<std::uint64_t>(material.lods.size()));
	for(const int lod : material.lods) writer.i32(lod);
	writeCanonicalPbrMetallicRoughness(writer, material.pbrMetallicRoughness);
	writeCanonicalNormalTextureInfo(writer, material.normalTexture);
	writeCanonicalOcclusionTextureInfo(writer, material.occlusionTexture);
	writeCanonicalTextureInfo(writer, material.emissiveTexture);
	writeCanonicalParameterMap(writer, material.values);
	writeCanonicalParameterMap(writer, material.additionalValues);
	writeCanonicalRecordTail(writer, material);
}

void writeCanonicalSnapshotReference(CanonicalStructuralWriter& writer,
	const SnapshotStableReference& reference)
{
	writer.u8(static_cast<std::uint8_t>(reference.kind));
	writer.i32(reference.sourceIndex);
	writeCanonicalId(writer, reference.id);
}

[[nodiscard]] std::vector<EditorObjectId> sortedCanonicalIds(const IdSet& ids)
{
	std::vector<EditorObjectId> result(ids.begin(), ids.end());
	std::sort(result.begin(), result.end());
	return result;
}

void writeCanonicalIdSet(CanonicalStructuralWriter& writer, const IdSet& ids)
{
	writeCanonicalIds(writer, sortedCanonicalIds(ids));
}

void writeCanonicalStringSet(CanonicalStructuralWriter& writer,
	const std::unordered_set<std::string>& values)
{
	std::vector<std::string_view> ordered;
	ordered.reserve(values.size());
	for(const std::string& value : values) ordered.push_back(value);
	std::sort(ordered.begin(), ordered.end());
	writer.u64(static_cast<std::uint64_t>(ordered.size()));
	for(const std::string_view value : ordered) writer.string(value);
}

void writeCanonicalStringList(CanonicalStructuralWriter& writer,
	const std::vector<std::string>& values)
{
	writer.u64(static_cast<std::uint64_t>(values.size()));
	for(const std::string& value : values) writer.string(value);
}

void writeCanonicalRootState(CanonicalStructuralWriter& writer,
	const detail::MutationSceneRootStatePlan& state)
{
	writeCanonicalId(writer, state.scene);
	writeCanonicalDoubles(writer, state.baselineTranslation);
	writeCanonicalDoubles(writer, state.baselineRotation);
	writeCanonicalDoubles(writer, state.baselineScale);
	writeCanonicalDoubles(writer, state.currentTranslation);
	writeCanonicalDoubles(writer, state.currentRotation);
	writeCanonicalDoubles(writer, state.currentScale);
	writer.u64(state.expectedExternalStateSerial);
	writer.u64(state.targetExternalStateSerial);
	writer.boolean(state.frozen);
	writer.boolean(state.preserveExternalOnMismatch);
	writeCanonicalIds(writer, state.frozenRootIds);
}

void writeCanonicalOwnedRuns(CanonicalStructuralWriter& writer,
	const std::vector<EditorObjectId>& ids,
	const IdSet& owned)
{
	std::vector<std::pair<std::size_t, std::size_t>> runs;
	for(std::size_t begin = 0; begin < ids.size();)
	{
		if(!owned.contains(ids[begin]))
		{
			++begin;
			continue;
		}
		std::size_t end = begin + 1u;
		while(end < ids.size() && owned.contains(ids[end])) ++end;
		runs.emplace_back(begin, end);
		begin = end;
	}
	writer.u64(static_cast<std::uint64_t>(runs.size()));
	for(std::size_t ordinal = 0; ordinal < runs.size(); ++ordinal)
	{
		const auto [begin, end] = runs[ordinal];
		writer.u64(static_cast<std::uint64_t>(ordinal));
		writer.u64(static_cast<std::uint64_t>(begin));
		writer.u64(static_cast<std::uint64_t>(end - begin));
		std::vector<EditorObjectId> run(ids.begin() + static_cast<std::ptrdiff_t>(begin),
			ids.begin() + static_cast<std::ptrdiff_t>(end));
		writeCanonicalIds(writer, run);
		std::vector<EditorObjectId> predecessors;
		for(std::size_t index = begin; index > 0; --index)
			if(!owned.contains(ids[index - 1u])) predecessors.push_back(ids[index - 1u]);
		writeCanonicalIds(writer, predecessors);
		std::vector<EditorObjectId> successors;
		for(std::size_t index = end; index < ids.size(); ++index)
			if(!owned.contains(ids[index])) successors.push_back(ids[index]);
		writeCanonicalIds(writer, successors);
	}
}

void writeCanonicalAnchoredList(CanonicalStructuralWriter& writer,
	std::uint8_t ownerKind,
	EditorObjectId owner,
	const std::vector<int>& membership,
	const Plan& plan,
	const IdSet& owned)
{
	writer.u8(ownerKind);
	writeCanonicalId(writer, owner);
	const auto ids = stableNodeList(membership, plan);
	writer.boolean(ids.has_value());
	if(!ids)
	{
		writeCanonicalReferences(writer, membership, EditorObjectKind::node, plan);
		writer.u64(0u);
		return;
	}
	writeCanonicalIds(writer, *ids);
	writeCanonicalOwnedRuns(writer, *ids, owned);
}

} // namespace

struct StructuralDelta::Impl final
{
	EditorDocumentAuthority authority;
	std::uint64_t bytes{0};
	std::uint64_t captureRevision{0};
	std::uint64_t captureSerial{0};
	std::uint8_t operationKind{0};
	StructuralSideStorage undo;
	StructuralSideStorage redo;
	std::array<IdSet, static_cast<std::size_t>(EditorObjectKind::count)> collectionOwned;
	std::array<IdSet, static_cast<std::size_t>(EditorObjectKind::count)> recordOwned;
	IdSet nodeMembershipOwned;
	std::unordered_set<std::string> extensionsUsedOwned;
	std::unordered_set<std::string> extensionsRequiredOwned;
};

struct PreparedStructuralMutation::Impl final
{
	EditorDocumentAuthority authority;
	MutationToken sourceToken;
	std::unique_ptr<tinygltf::Model> candidate;
	Plan plan;
	Plan planBefore;
	std::unique_ptr<StructuralDelta> delta;
	std::uint64_t bytes{0};
	std::string message;
	MutationStatus status{MutationStatus::applied};
	MutationErrorCode domainError{MutationErrorCode::none};
	bool consumed{false};
};

StructuralDelta::StructuralDelta(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

StructuralDelta::StructuralDelta(StructuralDelta&&) noexcept = default;
StructuralDelta& StructuralDelta::operator=(StructuralDelta&&) noexcept = default;
StructuralDelta::~StructuralDelta() = default;

bool StructuralDelta::valid() const noexcept
{
	return m_impl && m_impl->authority.valid() && m_impl->bytes != 0 && m_impl->undo.model
		&& m_impl->redo.model;
}

EditorDocumentAuthority StructuralDelta::authority() const noexcept
{
	return m_impl ? m_impl->authority : EditorDocumentAuthority{};
}

std::uint64_t StructuralDelta::snapshotBytes() const noexcept
{
	return m_impl ? m_impl->bytes : 0u;
}

StructuralDeltaCanonicalSide StructuralDelta::inspectCanonicalSide(
	StructuralState side) const noexcept
{
	StructuralDeltaCanonicalSide result;
	result.side = side;
	if(side != StructuralState::undo && side != StructuralState::redo)
	{
		result.status = StructuralDeltaInspectionStatus::invalidState;
		return result;
	}
	if(!valid()) return result;
	try
	{
		const StructuralSideStorage& selected =
			side == StructuralState::undo ? m_impl->undo : m_impl->redo;
		const StructuralSideStorage& other =
			side == StructuralState::undo ? m_impl->redo : m_impl->undo;
		CanonicalStructuralWriter writer;
		writer.reserve(static_cast<std::size_t>(
			std::min<std::uint64_t>(m_impl->bytes, std::numeric_limits<std::size_t>::max())));
		writer.raw("MGIFSDC1");
		writer.u32(StructuralDeltaCanonicalSide::kSchemaVersion);
		writer.u8(static_cast<std::uint8_t>(side));
		writer.u32(8u);

		std::size_t section = writer.beginSection(canonicalTag('D', 'I', 'A', 'G'));
		writer.u64(m_impl->authority.value);
		writer.u64(m_impl->bytes);
		writer.u64(m_impl->captureRevision);
		writer.u64(m_impl->captureSerial);
		writer.u8(m_impl->operationKind);
		writer.u64(selected.documentRevision);
		writer.u64(static_cast<std::uint64_t>(selected.sceneHierarchyEpochs.size()));
		for(const auto& [scene, epoch] : selected.sceneHierarchyEpochs)
		{
			writeCanonicalId(writer, scene);
			writer.u64(epoch);
		}
		writer.endSection(section);

		section = writer.beginSection(canonicalTag('R', 'E', 'G', 'I'));
		writer.u64(kindIndex(EditorObjectKind::count));
		for(std::size_t kind = 0; kind < kindIndex(EditorObjectKind::count); ++kind)
		{
			writer.u8(static_cast<std::uint8_t>(kind));
			writeCanonicalIds(writer, selected.plan.idsByKind[kind]);
			writeCanonicalOwnedRuns(
				writer, selected.plan.idsByKind[kind], m_impl->collectionOwned[kind]);
		}
		writer.endSection(section);

		section = writer.beginSection(canonicalTag('S', 'L', 'O', 'T'));
		writer.u64(kindIndex(EditorObjectKind::count));
		for(std::size_t kind = 0; kind < kindIndex(EditorObjectKind::count); ++kind)
		{
			writer.u8(static_cast<std::uint8_t>(kind));
			std::vector<EditorObjectId> slots = selected.plan.idsByKind[kind];
			slots.insert(
				slots.end(), other.plan.idsByKind[kind].begin(), other.plan.idsByKind[kind].end());
			std::sort(slots.begin(), slots.end());
			slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
			writer.u64(static_cast<std::uint64_t>(slots.size()));
			for(const EditorObjectId id : slots)
			{
				writeCanonicalId(writer, id);
				const auto selectedIndex = stableIndex(selected.plan.idsByKind[kind], id);
				const auto otherIndex = stableIndex(other.plan.idsByKind[kind], id);
				writer.boolean(selectedIndex.has_value());
				writer.u64(selectedIndex ? static_cast<std::uint64_t>(*selectedIndex)
										 : std::numeric_limits<std::uint64_t>::max());
				writer.boolean(otherIndex.has_value());
				writer.u64(otherIndex ? static_cast<std::uint64_t>(*otherIndex)
									  : std::numeric_limits<std::uint64_t>::max());
				writer.boolean(m_impl->collectionOwned[kind].contains(id));
				writer.boolean(m_impl->recordOwned[kind].contains(id));
				writer.boolean(kind == kindIndex(EditorObjectKind::node)
					&& m_impl->nodeMembershipOwned.contains(id));
			}
		}
		writer.endSection(section);

		section = writer.beginSection(canonicalTag('R', 'E', 'C', 'D'));
		const auto writeRecords =
			[&](EditorObjectKind kind, const auto& records, auto&& writeRecord)
		{
			writer.u8(static_cast<std::uint8_t>(kind));
			writer.u64(static_cast<std::uint64_t>(records.size()));
			const auto& ids = selected.plan.idsByKind[kindIndex(kind)];
			for(std::size_t index = 0; index < records.size(); ++index)
			{
				const std::size_t item = writer.beginSection(canonicalTag('I', 'T', 'E', 'M'));
				writeCanonicalId(writer, index < ids.size() ? ids[index] : EditorObjectId{});
				writeRecord(records[index]);
				writer.endSection(item);
			}
		};
		writer.u32(5u);
		writeRecords(EditorObjectKind::scene,
			selected.model->scenes,
			[&](const tinygltf::Scene& record)
			{ writeCanonicalScene(writer, record, selected.plan); });
		writeRecords(EditorObjectKind::node,
			selected.model->nodes,
			[&](const tinygltf::Node& record)
			{ writeCanonicalNode(writer, record, selected.plan); });
		writeRecords(EditorObjectKind::skin,
			selected.model->skins,
			[&](const tinygltf::Skin& record)
			{ writeCanonicalSkin(writer, record, selected.plan); });
		writeRecords(EditorObjectKind::animation,
			selected.model->animations,
			[&](const tinygltf::Animation& record)
			{ writeCanonicalAnimation(writer, record, selected.plan); });
		writeRecords(EditorObjectKind::light,
			selected.model->lights,
			[&](const tinygltf::Light& record) { writeCanonicalLight(writer, record); });
		writer.endSection(section);

		section = writer.beginSection(canonicalTag('M', 'E', 'M', 'B'));
		writer.u64(static_cast<std::uint64_t>(
			selected.model->scenes.size() + selected.model->nodes.size()));
		const auto& sceneIds = selected.plan.idsByKind[kindIndex(EditorObjectKind::scene)];
		for(std::size_t index = 0; index < selected.model->scenes.size(); ++index)
		{
			const std::size_t list = writer.beginSection(canonicalTag('L', 'I', 'S', 'T'));
			writeCanonicalAnchoredList(writer,
				1u,
				index < sceneIds.size() ? sceneIds[index] : EditorObjectId{},
				selected.model->scenes[index].nodes,
				selected.plan,
				m_impl->nodeMembershipOwned);
			writer.endSection(list);
		}
		const auto& nodeIds = selected.plan.idsByKind[kindIndex(EditorObjectKind::node)];
		for(std::size_t index = 0; index < selected.model->nodes.size(); ++index)
		{
			const std::size_t list = writer.beginSection(canonicalTag('L', 'I', 'S', 'T'));
			writeCanonicalAnchoredList(writer,
				2u,
				index < nodeIds.size() ? nodeIds[index] : EditorObjectId{},
				selected.model->nodes[index].children,
				selected.plan,
				m_impl->nodeMembershipOwned);
			writer.endSection(list);
		}
		writer.endSection(section);

		section = writer.beginSection(canonicalTag('P', 'L', 'A', 'N'));
		writeCanonicalId(writer, selected.plan.currentScene.untyped());
		writer.boolean(selected.plan.hierarchyChanged);
		writer.boolean(selected.plan.allowResurrection);
		writer.boolean(selected.plan.retainExternalStateSerial);
		writer.u64(static_cast<std::uint64_t>(selected.plan.primitiveAddresses.size()));
		const auto& primitiveIds = selected.plan.idsByKind[kindIndex(EditorObjectKind::primitive)];
		for(std::size_t index = 0; index < selected.plan.primitiveAddresses.size(); ++index)
		{
			writeCanonicalId(
				writer, index < primitiveIds.size() ? primitiveIds[index] : EditorObjectId{});
			writer.u32(selected.plan.primitiveAddresses[index].meshIndex);
			writer.u32(selected.plan.primitiveAddresses[index].primitiveIndex);
		}
		writer.u64(static_cast<std::uint64_t>(selected.plan.frozenSceneRootIndicesByScene.size()));
		for(const auto& roots : selected.plan.frozenSceneRootIndicesByScene)
		{
			writer.u64(static_cast<std::uint64_t>(roots.size()));
			for(const std::size_t root : roots)
			{
				if(root < nodeIds.size())
				{
					writer.u8(1u);
					writeCanonicalId(writer, nodeIds[root]);
				}
				else
				{
					writer.u8(2u);
					writer.u64(static_cast<std::uint64_t>(root));
				}
			}
		}
		writer.u64(static_cast<std::uint64_t>(selected.plan.sceneRootStates.size()));
		for(const auto& rootState : selected.plan.sceneRootStates)
			writeCanonicalRootState(writer, rootState);
		writer.endSection(section);

		section = writer.beginSection(canonicalTag('O', 'W', 'N', 'R'));
		writer.u64(kindIndex(EditorObjectKind::count));
		for(std::size_t kind = 0; kind < kindIndex(EditorObjectKind::count); ++kind)
		{
			writer.u8(static_cast<std::uint8_t>(kind));
			writeCanonicalIdSet(writer, m_impl->collectionOwned[kind]);
			writeCanonicalIdSet(writer, m_impl->recordOwned[kind]);
		}
		writeCanonicalIdSet(writer, m_impl->nodeMembershipOwned);
		writer.endSection(section);

		section = writer.beginSection(canonicalTag('E', 'X', 'T', 'N'));
		writeCanonicalStringList(writer, selected.model->extensionsUsed);
		writeCanonicalStringList(writer, selected.model->extensionsRequired);
		writeCanonicalStringSet(writer, m_impl->extensionsUsedOwned);
		writeCanonicalStringSet(writer, m_impl->extensionsRequiredOwned);
		writer.endSection(section);

		result.canonicalBytes = writer.take();
		result.status = StructuralDeltaInspectionStatus::complete;
		return result;
	}
	catch(...)
	{
		result.canonicalBytes.clear();
		result.status = StructuralDeltaInspectionStatus::allocationFailed;
		return result;
	}
}

PreparedStructuralMutation::PreparedStructuralMutation(std::unique_ptr<Impl> impl) noexcept
	: m_impl(std::move(impl))
{
}

PreparedStructuralMutation::PreparedStructuralMutation(
	PreparedStructuralMutation&&) noexcept = default;
PreparedStructuralMutation& PreparedStructuralMutation::operator=(
	PreparedStructuralMutation&&) noexcept = default;
PreparedStructuralMutation::~PreparedStructuralMutation() = default;

bool PreparedStructuralMutation::valid() const noexcept
{
	return m_impl && !m_impl->consumed && m_impl->authority.valid()
		&& (m_impl->status == MutationStatus::domainNoop
			|| (m_impl->candidate && m_impl->delta && m_impl->delta->valid()));
}

std::uint64_t PreparedStructuralMutation::snapshotBytes() const noexcept
{
	return m_impl ? m_impl->bytes : 0u;
}

HistoryPolicy PreparedStructuralMutation::historyPolicy() const noexcept
{
	return HistoryPolicy::undoable;
}

bool StructuralMutationResult::hasDelta() const noexcept
{
	return delta && delta->valid();
}

bool MaterialSnapshot::valid() const noexcept
{
	return m_impl && m_impl->id.valid() && m_impl->authority.valid()
		&& m_impl->id.authority() == m_impl->authority && m_impl->bytes != 0;
}

MaterialId MaterialSnapshot::id() const noexcept
{
	return m_impl ? m_impl->id : MaterialId{};
}

EditorDocumentAuthority MaterialSnapshot::authority() const noexcept
{
	return m_impl ? m_impl->authority : EditorDocumentAuthority{};
}

std::string_view MaterialSnapshot::name() const noexcept
{
	return m_impl ? std::string_view(m_impl->record.name) : std::string_view{};
}

std::uint64_t MaterialSnapshot::snapshotBytes() const noexcept
{
	return m_impl ? m_impl->bytes : 0u;
}

bool MaterialSnapshot::operator==(const MaterialSnapshot& other) const noexcept
{
	if(!m_impl || !other.m_impl) return m_impl == other.m_impl;
	return m_impl->id == other.m_impl->id && m_impl->authority == other.m_impl->authority
		&& m_impl->stableReferences == other.m_impl->stableReferences
		&& sameMaterialSnapshotRecord(m_impl->record, other.m_impl->record);
}

SnapshotCanonicalRecord MaterialSnapshot::inspectCanonicalRecord() const noexcept
{
	SnapshotCanonicalRecord result;
	result.kind = EditorObjectKind::material;
	if(!valid()) return result;
	try
	{
		CanonicalStructuralWriter writer;
		writer.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(
			m_impl->bytes, std::numeric_limits<std::size_t>::max())));
		writer.raw("MGIFSRC1");
		writer.u32(SnapshotCanonicalRecord::kSchemaVersion);
		writer.u8(static_cast<std::uint8_t>(result.kind));
		writer.u32(3u);

		auto section = writer.beginSection(canonicalTag('M', 'E', 'T', 'A'));
		writer.u64(m_impl->authority.value);
		writeCanonicalId(writer, m_impl->id.untyped());
		writer.endSection(section);

		section = writer.beginSection(canonicalTag('R', 'E', 'F', 'S'));
		writer.u64(static_cast<std::uint64_t>(m_impl->stableReferences.size()));
		for(const SnapshotStableReference& reference : m_impl->stableReferences)
			writeCanonicalSnapshotReference(writer, reference);
		writer.endSection(section);

		section = writer.beginSection(canonicalTag('R', 'E', 'C', 'D'));
		writeCanonicalMaterial(writer, m_impl->record);
		writer.endSection(section);

		result.canonicalBytes = writer.take();
		result.status = SnapshotInspectionStatus::complete;
		return result;
	}
	catch(...)
	{
		result.canonicalBytes.clear();
		result.status = SnapshotInspectionStatus::allocationFailed;
		return result;
	}
}

bool LightSnapshot::valid() const noexcept
{
	return m_impl && m_impl->id.valid() && m_impl->authority.valid()
		&& m_impl->id.authority() == m_impl->authority && m_impl->bytes != 0;
}

LightId LightSnapshot::id() const noexcept
{
	return m_impl ? m_impl->id : LightId{};
}

EditorDocumentAuthority LightSnapshot::authority() const noexcept
{
	return m_impl ? m_impl->authority : EditorDocumentAuthority{};
}

std::string_view LightSnapshot::name() const noexcept
{
	return m_impl ? std::string_view(m_impl->record.name) : std::string_view{};
}

std::uint64_t LightSnapshot::snapshotBytes() const noexcept { return m_impl ? m_impl->bytes : 0u; }

bool LightSnapshot::operator==(const LightSnapshot& other) const noexcept
{
	if(!m_impl || !other.m_impl) return m_impl == other.m_impl;
	return m_impl->id == other.m_impl->id && m_impl->authority == other.m_impl->authority
		&& sameLightSnapshotRecord(m_impl->record, other.m_impl->record);
}

SnapshotCanonicalRecord LightSnapshot::inspectCanonicalRecord() const noexcept
{
	SnapshotCanonicalRecord result;
	result.kind = EditorObjectKind::light;
	if(!valid()) return result;
	try
	{
		CanonicalStructuralWriter writer;
		writer.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(
			m_impl->bytes, std::numeric_limits<std::size_t>::max())));
		writer.raw("MGIFSRC1");
		writer.u32(SnapshotCanonicalRecord::kSchemaVersion);
		writer.u8(static_cast<std::uint8_t>(result.kind));
		writer.u32(3u);

		auto section = writer.beginSection(canonicalTag('M', 'E', 'T', 'A'));
		writer.u64(m_impl->authority.value);
		writeCanonicalId(writer, m_impl->id.untyped());
		writer.endSection(section);

		section = writer.beginSection(canonicalTag('R', 'E', 'F', 'S'));
		writer.u64(0u);
		writer.endSection(section);

		section = writer.beginSection(canonicalTag('R', 'E', 'C', 'D'));
		writeCanonicalLight(writer, m_impl->record);
		writer.endSection(section);

		result.canonicalBytes = writer.take();
		result.status = SnapshotInspectionStatus::complete;
		return result;
	}
	catch(...)
	{
		result.canonicalBytes.clear();
		result.status = SnapshotInspectionStatus::allocationFailed;
		return result;
	}
}

const std::vector<MutationRemapEntry>& MutationRemap::forKind(EditorObjectKind kind) const noexcept
{
	switch(kind)
	{
		case EditorObjectKind::scene: return scenes;
		case EditorObjectKind::node: return nodes;
		case EditorObjectKind::primitive: return primitives;
		case EditorObjectKind::mesh: return meshes;
		case EditorObjectKind::material: return materials;
		case EditorObjectKind::texture: return textures;
		case EditorObjectKind::image: return images;
		case EditorObjectKind::sampler: return samplers;
		case EditorObjectKind::accessor: return accessors;
		case EditorObjectKind::bufferView: return bufferViews;
		case EditorObjectKind::buffer: return buffers;
		case EditorObjectKind::skin: return skins;
		case EditorObjectKind::animation: return animations;
		case EditorObjectKind::camera: return cameras;
		case EditorObjectKind::light: return lights;
		default: return nodes;
	}
}

bool MaterialExtensionValue::operator==(const MaterialExtensionValue& other) const
{
	if(value.index() != other.value.index()) return false;
	if(const auto* left = std::get_if<Object>(&value))
	{
		const auto& right = std::get<Object>(other.value);
		if(left->size() != right.size()) return false;
		for(const auto& entry : *left)
		{
			const auto found = std::find_if(right.begin(), right.end(),
				[&entry](const auto& candidate) { return candidate.first == entry.first; });
			if(found == right.end() || !(entry.second == found->second)) return false;
		}
		return true;
	}
	if(const auto* left = std::get_if<Array>(&value))
	{
		const auto& right = std::get<Array>(other.value);
		return left->size() == right.size()
			&& std::equal(left->begin(), left->end(), right.begin());
	}
	return value == other.value;
}

SceneCommands::SceneCommands(GltfDocument& document, EditorLimits limits,
	EditorFaultInjector* faultInjector) noexcept
	: m_document(document), m_limits(limits), m_faultInjector(faultInjector),
	  m_serial(allocateMutationSerial())
{
}

MutationToken SceneCommands::token() const noexcept
{
	return MutationToken{m_document.documentAuthority(), m_document.revision(), m_serial};
}

MutationResult SceneCommands::query(EditorObjectId id, const MutationGuard& guard) const
{
	MutationResult result = baseResult(
		HistoryPolicy::notApplicable, m_document, guard, m_serial);
	if(result.error != MutationErrorCode::none) return result;
	if(!id.valid())
	{
		result.error = MutationErrorCode::invalidId;
		result.message = "object ID is invalid";
	}
	else if(id.authority != m_document.documentAuthority())
	{
		result.error = MutationErrorCode::crossDocument;
		result.message = "object ID belongs to another document";
	}
	else if(!m_document.contains(id))
	{
		result.error = MutationErrorCode::staleId;
		result.message = "object ID is stale";
	}
	else
	{
		result.status = MutationStatus::domainNoop;
		result.error = MutationErrorCode::none;
		result.message = "query has no document effect";
	}
	return result;
}

std::optional<SceneNodeOccurrence> SceneCommands::captureNodeOccurrence(
	SceneId scene, NodeId node) const
{
	if(scene.authority() != m_document.documentAuthority()
		|| node.authority() != m_document.documentAuthority()) return std::nullopt;
	const auto sceneIndex = m_document.indexOf(scene.untyped());
	const auto nodeIndex = m_document.indexOf(node.untyped());
	if(!sceneIndex || !nodeIndex) return std::nullopt;
	SceneStructureSnapshot snapshot;
	if(captureSceneStructure(
		detail::SceneCommandsAccess::model(m_document), m_limits, snapshot)
		!= MutationErrorCode::none) return std::nullopt;
	const auto parent = occurrenceParent(snapshot, *sceneIndex, *nodeIndex);
	if(!parent) return std::nullopt;
	SceneNodeOccurrence result;
	result.scene = scene;
	result.node = node;
	if(*parent >= 0)
	{
		result.parent = NodeId::from(m_document.idAt(
			EditorObjectKind::node, static_cast<std::size_t>(*parent)));
		if(!result.parent) return std::nullopt;
	}
	result.revision = m_document.revision();
	return result;
}

StructuralPrepareResult SceneCommands::prepareStructural(
	StructuralRequest request, const MutationGuard& guard) const
{
	StructuralPrepareResult output;
	output.historyPolicy = HistoryPolicy::undoable;
	MutationResult preflight = baseResult(
		HistoryPolicy::undoable, m_document, guard, m_serial);
	if(preflight.error != MutationErrorCode::none)
	{
		output.error = preflight.error;
		output.message = std::move(preflight.message);
		return output;
	}
	try
	{
		if(m_faultInjector && m_faultInjector->shouldFail(EditorFaultPoint::allocation))
		{
			output.error = MutationErrorCode::allocationFailed;
			output.message = "structural preparation allocation fault was injected";
			return output;
		}
		Plan beforePlan = makePlan(m_document);
		std::vector<std::pair<EditorObjectId, std::uint64_t>> beforeHierarchyEpochs;
		for(std::size_t sceneIndex = 0;
			sceneIndex < beforePlan.idsByKind[kindIndex(EditorObjectKind::scene)].size();
			++sceneIndex)
		{
			const auto scene = SceneId::from(
				beforePlan.idsByKind[kindIndex(EditorObjectKind::scene)][sceneIndex]);
			if(scene)
			{
				beforePlan.sceneRootStates.push_back(rootStatePlan(
					*scene, m_document.sceneRootTransformState(*scene),
					m_document.frozenSceneRootNodeIds(*scene), false));
				beforeHierarchyEpochs.emplace_back(
					scene->untyped(), m_document.sceneRootTransformState(*scene).hierarchyEpoch);
			}
		}
		auto beforeModel = std::make_unique<tinygltf::Model>(
			detail::SceneCommandsAccess::model(m_document));
		auto scratchModel = std::make_unique<tinygltf::Model>(*beforeModel);
		std::vector<GltfDependency> dependencies(
			m_document.dependencies().begin(), m_document.dependencies().end());
		auto scratch = std::unique_ptr<GltfDocument>(new GltfDocument(
			std::move(scratchModel), m_document.sourcePath(), m_document.format(),
			std::move(dependencies)));
		SceneCommands scratchCommands(*scratch, m_limits, nullptr);

		const auto mapUntyped = [&](EditorObjectId source) -> EditorObjectId
		{
			const auto index = m_document.indexOf(source);
			return index ? scratch->idAt(source.kind, *index) : EditorObjectId{};
		};
		const auto mapScene = [&](SceneId source) -> std::optional<SceneId>
		{
			return SceneId::from(mapUntyped(source.untyped()));
		};
		const auto mapNode = [&](NodeId source) -> std::optional<NodeId>
		{
			return NodeId::from(mapUntyped(source.untyped()));
		};
		const auto mapOccurrence = [&](const SceneNodeOccurrence& source)
			-> std::optional<SceneNodeOccurrence>
		{
			const auto scene = mapScene(source.scene);
			const auto node = mapNode(source.node);
			if(!scene || !node) return std::nullopt;
			auto mapped = scratchCommands.captureNodeOccurrence(*scene, *node);
			if(!mapped) return std::nullopt;
			if(source.parent)
			{
				const auto parent = mapNode(*source.parent);
				if(!parent || mapped->parent != parent) return std::nullopt;
			}
			else if(mapped->parent) return std::nullopt;
			return mapped;
		};

		MutationResult scratchResult;
		bool mapped = true;
		std::visit([&](auto& value)
		{
			using T = std::decay_t<decltype(value)>;
			if constexpr(std::is_same_v<T, AddChildNodeRequest>)
			{
				const auto scene = mapScene(value.scene);
				const auto parent = mapNode(value.parent);
				mapped = scene && parent;
				if(mapped) scratchResult = scratchCommands.addNode(
					*scene, std::move(value.name), *parent);
			}
			else if constexpr(std::is_same_v<T, AddLightRequest>)
			{
				const auto scene = mapScene(value.scene);
				std::optional<NodeId> parent;
				if(value.parent) parent = mapNode(*value.parent);
				mapped = scene && (!value.parent || parent);
				if(mapped) scratchResult = scratchCommands.createLight(
					*scene, parent, std::move(value.nodeName),
					std::move(value.lightName), value.properties);
			}
			else if constexpr(std::is_same_v<T, DuplicateNodeRequest>)
			{
				const auto source = mapOccurrence(value.source);
				std::optional<SceneNodeOccurrence> parent;
				if(value.parent) parent = mapOccurrence(*value.parent);
				mapped = source && (!value.parent || parent);
				if(mapped) scratchResult = scratchCommands.duplicateNodeAt(
					*source, parent, value.policy);
			}
			else if constexpr(std::is_same_v<T, DeleteNodeRequest>)
			{
				const auto occurrence = mapOccurrence(value.occurrence);
				mapped = occurrence.has_value();
				if(mapped) scratchResult = scratchCommands.deleteNodeAt(
					*occurrence, value.policy);
			}
			else
			{
				const auto source = mapOccurrence(value.source);
				std::optional<SceneNodeOccurrence> parent;
				if(value.parent) parent = mapOccurrence(*value.parent);
				const SceneId requestedTarget = value.targetScene.valid()
					? value.targetScene
					: value.parent ? value.parent->scene : value.source.scene;
				const auto target = mapScene(requestedTarget);
				mapped = source && (!value.parent || parent) && target;
				if(mapped) scratchResult = scratchCommands.reparentNodeAt(
					*source, parent, value.policy, *target);
			}
		}, request);

		if(!mapped)
		{
			output.error = MutationErrorCode::staleOccurrence;
			output.message = "structural request contains a stale or foreign occurrence";
			return output;
		}
		if(scratchResult.rejected())
		{
			output.error = scratchResult.error;
			output.message = std::move(scratchResult.message);
			return output;
		}

		auto preparedImpl = std::make_unique<PreparedStructuralMutation::Impl>();
		preparedImpl->authority = m_document.documentAuthority();
		preparedImpl->sourceToken = token();
		preparedImpl->planBefore = beforePlan;
		preparedImpl->message = scratchResult.message;
		preparedImpl->status = scratchResult.status;
		preparedImpl->domainError = scratchResult.error;
		if(scratchResult.domainNoop())
		{
			preparedImpl->bytes = 0;
			output.prepared = std::unique_ptr<PreparedStructuralMutation>(
				new PreparedStructuralMutation(std::move(preparedImpl)));
			return output;
		}

		preparedImpl->candidate = scratch->takeAuthorityModel();
		Plan afterPlan;
		for(std::size_t kind = 1; kind < kindIndex(EditorObjectKind::count); ++kind)
		{
			const EditorObjectKind type = static_cast<EditorObjectKind>(kind);
			afterPlan.idsByKind[kind].reserve(scratch->objectCount(type));
			for(std::size_t index = 0; index < scratch->objectCount(type); ++index)
			{
				const EditorObjectId scratchId = scratch->idAt(type, index);
				afterPlan.idsByKind[kind].push_back(
					scratchId.slot < beforePlan.idsByKind[kind].size()
						? beforePlan.idsByKind[kind][scratchId.slot]
						: EditorObjectId{});
			}
		}
		for(std::size_t index = 0;
			index < scratch->objectCount(EditorObjectKind::primitive); ++index)
		{
			const auto id = PrimitiveId::from(scratch->idAt(EditorObjectKind::primitive, index));
			if(id)
			{
				const auto address = scratch->primitiveAddress(*id);
				if(address) afterPlan.primitiveAddresses.push_back(*address);
			}
		}
		afterPlan.currentScene = m_document.currentSceneId();
		afterPlan.hierarchyChanged = true;
		afterPlan.retainExternalStateSerial = true;
		const MutationErrorCode bakeError = bakeSceneRootPreviews(
			m_document, *preparedImpl->candidate, afterPlan);
		if(bakeError != MutationErrorCode::none)
		{
			output.error = bakeError;
			output.message = "scene-root preview cannot be frozen consistently";
			return output;
		}
		const detail::MutationCommitStatus idStatus =
			detail::SceneCommandsAccess::resolvePreparedIds(m_document, afterPlan);
		if(idStatus != detail::MutationCommitStatus::applied)
		{
			output.error = commitStatusError(idStatus);
			output.message = "structural stable IDs cannot be prepared";
			return output;
		}
		for(std::size_t sceneIndex = 0;
			sceneIndex < afterPlan.idsByKind[kindIndex(EditorObjectKind::scene)].size();
			++sceneIndex)
		{
			const EditorObjectId sceneId =
				afterPlan.idsByKind[kindIndex(EditorObjectKind::scene)][sceneIndex];
			if(!sceneId.valid()) continue;
			detail::MutationSceneRootStatePlan state;
			state.scene = sceneId;
			const auto beforeState = std::find_if(beforePlan.sceneRootStates.begin(),
				beforePlan.sceneRootStates.end(), [&](const auto& value)
				{
					return value.scene == sceneId;
				});
			if(beforeState != beforePlan.sceneRootStates.end())
			{
				state.expectedExternalStateSerial = beforeState->targetExternalStateSerial;
				state.targetExternalStateSerial = beforeState->targetExternalStateSerial;
			}
			if(sceneIndex < afterPlan.frozenSceneRootIndicesByScene.size())
				for(const std::size_t root : afterPlan.frozenSceneRootIndicesByScene[sceneIndex])
					if(root < afterPlan.idsByKind[kindIndex(EditorObjectKind::node)].size())
						state.frozenRootIds.push_back(
							afterPlan.idsByKind[kindIndex(EditorObjectKind::node)][root]);
			state.frozen = sceneIndex < afterPlan.frozenSceneRootIndicesByScene.size()
				&& !afterPlan.frozenSceneRootIndicesByScene[sceneIndex].empty();
			afterPlan.sceneRootStates.push_back(std::move(state));
		}
		const GltfValidationReport validation = detail::SceneCommandsAccess::validateModel(
			*preparedImpl->candidate, m_limits);
		if(!validation.valid())
		{
			output.error = MutationErrorCode::validationFailed;
			output.message = validation.issues().empty()
				? "prepared structural candidate failed validation"
				: validation.issues().front().message;
			return output;
		}

		auto deltaImpl = std::make_unique<StructuralDelta::Impl>();
		deltaImpl->authority = m_document.documentAuthority();
		deltaImpl->captureRevision = m_document.revision();
		deltaImpl->captureSerial = m_serial;
		deltaImpl->operationKind = static_cast<std::uint8_t>(request.index());
		if(const auto* reparent = std::get_if<ReparentNodeRequest>(&request))
		{
			const auto sourceIndex =
				stableIndex(beforePlan.idsByKind[kindIndex(EditorObjectKind::node)],
					reparent->source.node.untyped());
			const auto moveToTail = [&](std::vector<int>& membership) noexcept
			{
				if(!sourceIndex
					|| *sourceIndex > static_cast<std::size_t>(std::numeric_limits<int>::max()))
					return false;
				const int source = static_cast<int>(*sourceIndex);
				const auto found = std::find(membership.begin(), membership.end(), source);
				if(found == membership.end()
					|| std::find(found + 1, membership.end(), source) != membership.end())
					return false;
				membership.erase(found);
				membership.push_back(source);
				return true;
			};
			bool reordered = false;
			if(reparent->source.parent)
			{
				const auto parentIndex =
					stableIndex(beforePlan.idsByKind[kindIndex(EditorObjectKind::node)],
						reparent->source.parent->untyped());
				reordered = parentIndex && *parentIndex < beforeModel->nodes.size()
					&& moveToTail(beforeModel->nodes[*parentIndex].children);
			}
			else if(reparent->policy == SceneOccurrencePolicy::clickedScene)
			{
				const auto sceneIndex =
					stableIndex(beforePlan.idsByKind[kindIndex(EditorObjectKind::scene)],
						reparent->source.scene.untyped());
				reordered = sceneIndex && *sceneIndex < beforeModel->scenes.size()
					&& moveToTail(beforeModel->scenes[*sceneIndex].nodes);
			}
			else
			{
				for(auto& scene : beforeModel->scenes)
				{
					if(!sourceIndex
						|| *sourceIndex > static_cast<std::size_t>(std::numeric_limits<int>::max()))
						break;
					const int source = static_cast<int>(*sourceIndex);
					if(std::find(scene.nodes.begin(), scene.nodes.end(), source)
						!= scene.nodes.end())
					{
						reordered = moveToTail(scene.nodes) || reordered;
					}
				}
			}
			if(!reordered)
			{
				output.error = MutationErrorCode::internalError;
				output.message = "prepared Reparent source membership "
								 "cannot be canonicalized";
				return output;
			}
		}
		deltaImpl->undo.model = std::move(beforeModel);
		deltaImpl->undo.plan = beforePlan;
		deltaImpl->undo.documentRevision = m_document.revision();
		deltaImpl->undo.sceneHierarchyEpochs = beforeHierarchyEpochs;
		deltaImpl->redo.model = std::make_unique<tinygltf::Model>(*preparedImpl->candidate);
		deltaImpl->redo.plan = afterPlan;
		deltaImpl->redo.documentRevision = nextRevisionWithoutWrap(m_document.revision());
		deltaImpl->redo.sceneHierarchyEpochs = beforeHierarchyEpochs;
		for(auto& [scene, epoch] : deltaImpl->redo.sceneHierarchyEpochs)
		{
			(void)scene;
			epoch = nextRevisionWithoutWrap(epoch);
		}
		for(std::size_t kind = 1; kind < kindIndex(EditorObjectKind::count); ++kind)
		{
			deltaImpl->collectionOwned[kind] = symmetricOwned(
				deltaImpl->undo.plan.idsByKind[kind], deltaImpl->redo.plan.idsByKind[kind]);
		}
		deltaImpl->nodeMembershipOwned =
			deltaImpl->collectionOwned[kindIndex(EditorObjectKind::node)];
		const auto captureMembership = [&](const std::vector<int>& before,
										   const Plan& beforeMembershipPlan,
										   const std::vector<int>& after,
										   const Plan& afterMembershipPlan)
		{
			const auto beforeIds = stableNodeList(before, beforeMembershipPlan);
			const auto afterIds = stableNodeList(after, afterMembershipPlan);
			if(beforeIds && afterIds)
				addMembershipDifference(*beforeIds, *afterIds, deltaImpl->nodeMembershipOwned);
		};
		const auto& undoScenes = deltaImpl->undo.plan.idsByKind[kindIndex(EditorObjectKind::scene)];
		const auto& redoScenes = deltaImpl->redo.plan.idsByKind[kindIndex(EditorObjectKind::scene)];
		for(const EditorObjectId scene : undoScenes)
		{
			const auto before = stableIndex(undoScenes, scene);
			const auto after = stableIndex(redoScenes, scene);
			if(before && after)
				captureMembership(deltaImpl->undo.model->scenes[*before].nodes,
					deltaImpl->undo.plan,
					deltaImpl->redo.model->scenes[*after].nodes,
					deltaImpl->redo.plan);
		}
		const auto& undoNodes = deltaImpl->undo.plan.idsByKind[kindIndex(EditorObjectKind::node)];
		const auto& redoNodes = deltaImpl->redo.plan.idsByKind[kindIndex(EditorObjectKind::node)];
		for(const EditorObjectId node : undoNodes)
		{
			const auto before = stableIndex(undoNodes, node);
			const auto after = stableIndex(redoNodes, node);
			if(!before || !after) continue;
			if(!sameNodeRecordWithoutMembership(
				   deltaImpl->undo.model->nodes[*before], deltaImpl->redo.model->nodes[*after]))
				deltaImpl->recordOwned[kindIndex(EditorObjectKind::node)].insert(node);
			captureMembership(deltaImpl->undo.model->nodes[*before].children,
				deltaImpl->undo.plan,
				deltaImpl->redo.model->nodes[*after].children,
				deltaImpl->redo.plan);
		}
		const auto captureRecordOwnership = [&]<typename Record>(EditorObjectKind kind,
												const std::vector<Record>& beforeRecords,
												const std::vector<Record>& afterRecords)
		{
			const auto& beforeIds = deltaImpl->undo.plan.idsByKind[kindIndex(kind)];
			const auto& afterIds = deltaImpl->redo.plan.idsByKind[kindIndex(kind)];
			for(const EditorObjectId id : beforeIds)
			{
				const auto before = stableIndex(beforeIds, id);
				const auto after = stableIndex(afterIds, id);
				if(before && after && !(beforeRecords[*before] == afterRecords[*after]))
					deltaImpl->recordOwned[kindIndex(kind)].insert(id);
			}
		};
		captureRecordOwnership(
			EditorObjectKind::skin, deltaImpl->undo.model->skins, deltaImpl->redo.model->skins);
		captureRecordOwnership(EditorObjectKind::animation,
			deltaImpl->undo.model->animations,
			deltaImpl->redo.model->animations);
		captureRecordOwnership(
			EditorObjectKind::light, deltaImpl->undo.model->lights, deltaImpl->redo.model->lights);
		const auto captureExtensionOwnership = [](const std::vector<std::string>& before,
												   const std::vector<std::string>& after,
												   std::unordered_set<std::string>& owned)
		{
			for(const std::string& name : before)
				if(std::find(after.begin(), after.end(), name) == after.end()) owned.insert(name);
			for(const std::string& name : after)
				if(std::find(before.begin(), before.end(), name) == before.end())
					owned.insert(name);
		};
		captureExtensionOwnership(deltaImpl->undo.model->extensionsUsed,
			deltaImpl->redo.model->extensionsUsed,
			deltaImpl->extensionsUsedOwned);
		captureExtensionOwnership(deltaImpl->undo.model->extensionsRequired,
			deltaImpl->redo.model->extensionsRequired,
			deltaImpl->extensionsRequiredOwned);
		stripStructuralPayloads(*deltaImpl->undo.model);
		stripStructuralPayloads(*deltaImpl->redo.model);
		const auto undoBytes = structuralModelBytes(*deltaImpl->undo.model);
		const auto redoBytes = structuralModelBytes(*deltaImpl->redo.model);
		const auto undoPlanBytes = structuralPlanBytes(deltaImpl->undo.plan);
		const auto redoPlanBytes = structuralPlanBytes(deltaImpl->redo.plan);
		std::uint64_t snapshotBytes =
			sizeof(StructuralDelta) + sizeof(StructuralDelta::Impl) + kStructuralDeltaControlCharge;
		bool ownershipAccounted = true;
		for(std::size_t kind = 0; kind < kindIndex(EditorObjectKind::count); ++kind)
			ownershipAccounted = ownershipAccounted
				&& addIdSetBytes(snapshotBytes, deltaImpl->collectionOwned[kind])
				&& addIdSetBytes(snapshotBytes, deltaImpl->recordOwned[kind]);
		ownershipAccounted = ownershipAccounted
			&& addIdSetBytes(snapshotBytes, deltaImpl->nodeMembershipOwned)
			&& addStringSetBytes(snapshotBytes, deltaImpl->extensionsUsedOwned)
			&& addStringSetBytes(snapshotBytes, deltaImpl->extensionsRequiredOwned)
			&& addVectorCapacityBytes(snapshotBytes, deltaImpl->undo.sceneHierarchyEpochs)
			&& addVectorCapacityBytes(snapshotBytes, deltaImpl->redo.sceneHierarchyEpochs);
		if(!undoBytes || !redoBytes || !undoPlanBytes || !redoPlanBytes || !ownershipAccounted
			|| !addSnapshotBytes(snapshotBytes, *undoBytes)
			|| !addSnapshotBytes(snapshotBytes, *redoBytes)
			|| !addSnapshotBytes(snapshotBytes, *undoPlanBytes)
			|| !addSnapshotBytes(snapshotBytes, *redoPlanBytes))
		{
			output.error = MutationErrorCode::snapshotBudgetExceeded;
			output.message = "structural snapshot byte count overflowed";
			return output;
		}
		if(snapshotBytes > m_limits.maxHistoryEntrySnapshotBytes)
		{
			output.error = MutationErrorCode::snapshotBudgetExceeded;
			output.message = "structural snapshot exceeds the per-entry budget";
			return output;
		}
		deltaImpl->bytes = snapshotBytes;
		preparedImpl->bytes = snapshotBytes;
		preparedImpl->plan = std::move(afterPlan);
		preparedImpl->delta =
			std::unique_ptr<StructuralDelta>(new StructuralDelta(std::move(deltaImpl)));
		output.prepared = std::unique_ptr<PreparedStructuralMutation>(
			new PreparedStructuralMutation(std::move(preparedImpl)));
		return output;
	}
	catch(const std::bad_alloc&)
	{
		output.error = MutationErrorCode::allocationFailed;
		output.message = "structural preparation allocation failed";
		return output;
	}
	catch(...)
	{
		output.error = MutationErrorCode::internalError;
		output.message = "structural preparation failed";
		return output;
	}
}

StructuralMutationResult SceneCommands::commitStructural(
	std::unique_ptr<PreparedStructuralMutation> prepared,
	const MutationGuard& guard)
{
	StructuralMutationResult output;
	output.mutation = baseResult(
		HistoryPolicy::undoable, m_document, guard, m_serial);
	if(output.mutation.error != MutationErrorCode::none) return output;
	if(!prepared || !prepared->valid() || !prepared->m_impl)
	{
		output.mutation.error = MutationErrorCode::staleTransaction;
		output.mutation.message = "structural preparation is invalid or consumed";
		return output;
	}
	auto& value = *prepared->m_impl;
	if(value.authority != m_document.documentAuthority()
		|| value.sourceToken != token())
	{
		output.mutation.error = value.authority != m_document.documentAuthority()
			? MutationErrorCode::crossDocument : MutationErrorCode::staleTransaction;
		output.mutation.message = "structural preparation is stale or foreign";
		return output;
	}
	value.consumed = true;
	if(value.status == MutationStatus::domainNoop)
	{
		output.mutation.status = MutationStatus::domainNoop;
		output.mutation.error = value.domainError;
		output.mutation.message = std::move(value.message);
		return output;
	}

	const std::uint64_t nextSerial = allocateMutationSerial();
	if(nextSerial == 0)
	{
		output.mutation.error = MutationErrorCode::internalError;
		output.mutation.message = "mutation serial space is exhausted";
		return output;
	}
	struct CommitContext final
	{
		MutationResult* result;
		const Plan* before;
		const tinygltf::Model* beforeModel;
		const tinygltf::Model* afterModel;
		StructuralDelta* delta;
		EditorDocumentAuthority authority;
		std::uint64_t baseRevision;
		std::uint64_t revision;
		std::uint64_t baseActivationEpoch;
		std::uint64_t serial;
	} context{&output.mutation, &value.planBefore,
		&detail::SceneCommandsAccess::model(m_document), value.candidate.get(),
		value.delta.get(), m_document.documentAuthority(), m_document.revision(),
		nextRevisionWithoutWrap(m_document.revision()),
		m_document.sceneActivationEpoch(), nextSerial};
	const auto prePublish = [](const Plan& resolved, void* opaque)
	{
		auto& context = *static_cast<CommitContext*>(opaque);
		context.result->status = MutationStatus::applied;
		context.result->error = MutationErrorCode::none;
		context.result->afterRevision = context.revision;
		context.result->transactionToken = MutationToken{
			context.authority, context.revision, context.serial};
		context.delta->m_impl->redo.plan = resolved;
		for(auto& rootState : context.delta->m_impl->redo.plan.sceneRootStates)
		{
			const auto scene = stableIndex(
				resolved.idsByKind[kindIndex(EditorObjectKind::scene)], rootState.scene);
			if(!scene || *scene >= resolved.frozenSceneRootIndicesByScene.size()) continue;
			rootState.frozenRootIds.clear();
			for(const std::size_t root : resolved.frozenSceneRootIndicesByScene[*scene])
				if(root < resolved.idsByKind[kindIndex(EditorObjectKind::node)].size())
					rootState.frozenRootIds.push_back(
						resolved.idsByKind[kindIndex(EditorObjectKind::node)][root]);
			rootState.frozen = !rootState.frozenRootIds.empty();
		}
		for(std::size_t kind = 1; kind < kindIndex(EditorObjectKind::count); ++kind)
		{
			addRemap(*context.result, context.before->idsByKind[kind],
				resolved.idsByKind[kind], static_cast<EditorObjectKind>(kind));
		}
		populateExactEvidence(*context.result, *context.beforeModel, *context.afterModel,
			*context.before, resolved, context.authority, context.baseRevision,
			context.revision, context.serial);
		if(context.before->currentScene != resolved.currentScene)
		{
			context.result->sceneActivationTransition = SceneActivationResult{
				SceneActivationStatus::Changed,
				context.before->currentScene.valid()
					? std::optional<SceneId>{context.before->currentScene} : std::nullopt,
				resolved.currentScene.valid()
					? std::optional<SceneId>{resolved.currentScene} : std::nullopt,
				context.baseActivationEpoch, context.baseActivationEpoch + 1u};
		}
	};
	detail::MutationCommitStatus status = detail::MutationCommitStatus::internalError;
	if(!detail::SceneCommandsAccess::commit(m_document, std::move(value.candidate),
		value.plan, prePublish, &context, status))
	{
		output.mutation.status = MutationStatus::rejected;
		output.mutation.afterRevision = output.mutation.beforeRevision;
		clearPublishedEvidence(output.mutation);
		output.mutation.transactionToken = token();
		output.mutation.error = commitStatusError(status);
		output.mutation.message = "structural commit failed before publication";
		return output;
	}
	m_serial = nextSerial;
	output.mutation.message = std::move(value.message);
	output.delta = std::move(value.delta);
	return output;
}

StructuralMutationResult SceneCommands::executeStructural(
	StructuralRequest request, const MutationGuard& guard)
{
	StructuralPrepareResult prepared = prepareStructural(std::move(request), guard);
	if(!prepared.succeeded())
	{
		StructuralMutationResult output;
		output.mutation = baseResult(
			HistoryPolicy::undoable, m_document, guard, m_serial);
		output.mutation.error = prepared.error;
		output.mutation.message = std::move(prepared.message);
		return output;
	}
	prepared.prepared->m_impl->plan.retainExternalStateSerial = false;
	return commitStructural(std::move(prepared.prepared), guard);
}

MutationResult SceneCommands::restoreStructural(
	const StructuralDelta& delta, StructuralState target, const MutationGuard& guard)
{
	MutationResult result = baseResult(
		HistoryPolicy::undoable, m_document, guard, m_serial);
	if(result.error != MutationErrorCode::none) return result;
	if(!delta.valid())
	{
		result.error = MutationErrorCode::invalidSnapshot;
		result.message = "structural delta is invalid";
		return result;
	}
	if(delta.authority() != m_document.documentAuthority())
	{
		result.error = MutationErrorCode::crossDocument;
		result.message = "structural delta belongs to another document";
		return result;
	}
	try
	{
		const StructuralSideStorage& source = target == StructuralState::undo
			? delta.m_impl->redo : delta.m_impl->undo;
		const StructuralSideStorage& destination = target == StructuralState::undo
			? delta.m_impl->undo : delta.m_impl->redo;
		const Plan current = makePlan(m_document);
		auto currentModel = std::make_unique<tinygltf::Model>(
			detail::SceneCommandsAccess::model(m_document));
		Plan plan = current;
		std::array<std::vector<EditorObjectId>,
			static_cast<std::size_t>(EditorObjectKind::count)> finalIds = current.idsByKind;
		for(const EditorObjectKind kind : {
			EditorObjectKind::node, EditorObjectKind::skin,
			EditorObjectKind::animation, EditorObjectKind::light})
		{
			const std::size_t index = kindIndex(kind);
			const auto composed = composeAnchored(current.idsByKind[index],
				source.plan.idsByKind[index], destination.plan.idsByKind[index],
				delta.m_impl->collectionOwned[index]);
			if(!composed)
			{
				result.error = MutationErrorCode::staleTransaction;
				result.message = "structural collection anchors no longer compose";
				return result;
			}
			finalIds[index] = *composed;
			plan.idsByKind[index] = *composed;
		}

		const auto validateNodeRecord = [&](EditorObjectId id) -> bool
		{
			const auto sourceIndex = stableIndex(
				source.plan.idsByKind[kindIndex(EditorObjectKind::node)], id);
			const auto currentIndex = stableIndex(
				current.idsByKind[kindIndex(EditorObjectKind::node)], id);
			if(!sourceIndex) return !currentIndex;
			return currentIndex && sameNodeRecordWithoutMembership(
				currentModel->nodes[*currentIndex], source.model->nodes[*sourceIndex]);
		};
		for(const EditorObjectId id : delta.m_impl->collectionOwned[kindIndex(EditorObjectKind::node)])
			if(!validateNodeRecord(id))
			{
				result.error = MutationErrorCode::staleTransaction;
				result.message = "an owned node record no longer matches the delta source";
				return result;
			}
		for(const EditorObjectId id : delta.m_impl->recordOwned[kindIndex(EditorObjectKind::node)])
			if(!validateNodeRecord(id))
			{
				result.error = MutationErrorCode::staleTransaction;
				result.message = "an owned node record no longer matches the delta source";
				return result;
			}

		const auto validateOwnedRecords = [&]<typename Record>(
			EditorObjectKind kind, const std::vector<Record>& currentRecords,
			const std::vector<Record>& sourceRecords) -> bool
		{
			IdSet owned = delta.m_impl->collectionOwned[kindIndex(kind)];
			owned.insert(delta.m_impl->recordOwned[kindIndex(kind)].begin(),
				delta.m_impl->recordOwned[kindIndex(kind)].end());
			for(const EditorObjectId id : owned)
			{
				const auto expected = stableIndex(source.plan.idsByKind[kindIndex(kind)], id);
				const auto actual = stableIndex(current.idsByKind[kindIndex(kind)], id);
				if(!expected)
				{
					if(actual) return false;
				}
				else if(!actual || !(currentRecords[*actual] == sourceRecords[*expected]))
					return false;
			}
			return true;
		};
		if(!validateOwnedRecords(EditorObjectKind::skin,
				currentModel->skins, source.model->skins)
			|| !validateOwnedRecords(EditorObjectKind::animation,
				currentModel->animations, source.model->animations)
			|| !validateOwnedRecords(EditorObjectKind::light,
				currentModel->lights, source.model->lights))
		{
			result.error = MutationErrorCode::staleTransaction;
			result.message = "an owned structural record no longer matches the delta source";
			return result;
		}

		auto candidate = std::make_unique<tinygltf::Model>(*currentModel);
		candidate->nodes.clear();
		candidate->nodes.reserve(finalIds[kindIndex(EditorObjectKind::node)].size());
		std::vector<bool> nodeFromTarget;
		nodeFromTarget.reserve(finalIds[kindIndex(EditorObjectKind::node)].size());
		for(const EditorObjectId id : finalIds[kindIndex(EditorObjectKind::node)])
		{
			tinygltf::Node record;
			bool fromTarget = false;
			if(!chooseStructuralRecord(id,
				current.idsByKind[kindIndex(EditorObjectKind::node)], currentModel->nodes,
				source.plan.idsByKind[kindIndex(EditorObjectKind::node)], source.model->nodes,
				destination.plan.idsByKind[kindIndex(EditorObjectKind::node)], destination.model->nodes,
				delta.m_impl->collectionOwned[kindIndex(EditorObjectKind::node)],
				delta.m_impl->recordOwned[kindIndex(EditorObjectKind::node)],
				record, fromTarget))
			{
				result.error = MutationErrorCode::staleTransaction;
				result.message = "a composed node record is unavailable";
				return result;
			}
			candidate->nodes.push_back(std::move(record));
			nodeFromTarget.push_back(fromTarget);
		}

		const auto rebuildRecords = [&]<typename Record>(
			EditorObjectKind kind, const std::vector<Record>& currentRecords,
			const std::vector<Record>& sourceRecords,
			const std::vector<Record>& targetRecords,
			std::vector<Record>& output, std::vector<bool>& origins) -> bool
		{
			output.clear();
			output.reserve(finalIds[kindIndex(kind)].size());
			origins.clear();
			origins.reserve(finalIds[kindIndex(kind)].size());
			for(const EditorObjectId id : finalIds[kindIndex(kind)])
			{
				Record record;
				bool fromTarget = false;
				if(!chooseStructuralRecord(id,
					current.idsByKind[kindIndex(kind)], currentRecords,
					source.plan.idsByKind[kindIndex(kind)], sourceRecords,
					destination.plan.idsByKind[kindIndex(kind)], targetRecords,
					delta.m_impl->collectionOwned[kindIndex(kind)],
					delta.m_impl->recordOwned[kindIndex(kind)], record, fromTarget))
					return false;
				output.push_back(std::move(record));
				origins.push_back(fromTarget);
			}
			return true;
		};
		std::vector<bool> skinFromTarget, animationFromTarget, lightFromTarget;
		if(!rebuildRecords(EditorObjectKind::skin, currentModel->skins, source.model->skins,
				destination.model->skins, candidate->skins, skinFromTarget)
			|| !rebuildRecords(EditorObjectKind::animation, currentModel->animations,
				source.model->animations, destination.model->animations,
				candidate->animations, animationFromTarget)
			|| !rebuildRecords(EditorObjectKind::light, currentModel->lights, source.model->lights,
				destination.model->lights, candidate->lights, lightFromTarget))
		{
			result.error = MutationErrorCode::staleTransaction;
			result.message = "a composed structural record is unavailable";
			return result;
		}

		const auto& finalNodes = finalIds[kindIndex(EditorObjectKind::node)];
		for(std::size_t index = 0; index < finalNodes.size(); ++index)
		{
			const EditorObjectId parent = finalNodes[index];
			const auto currentParent = stableIndex(
				current.idsByKind[kindIndex(EditorObjectKind::node)], parent);
			const auto sourceParent = stableIndex(
				source.plan.idsByKind[kindIndex(EditorObjectKind::node)], parent);
			const auto targetParent = stableIndex(
				destination.plan.idsByKind[kindIndex(EditorObjectKind::node)], parent);
			std::vector<EditorObjectId> currentChildren;
			std::vector<EditorObjectId> sourceChildren;
			std::vector<EditorObjectId> targetChildren;
			if(currentParent)
			{
				const auto values = stableNodeList(currentModel->nodes[*currentParent].children, current);
				if(!values) { result.error = MutationErrorCode::staleTransaction; return result; }
				currentChildren = *values;
			}
			if(sourceParent)
			{
				const auto values = stableNodeList(source.model->nodes[*sourceParent].children, source.plan);
				if(!values) { result.error = MutationErrorCode::staleTransaction; return result; }
				sourceChildren = *values;
			}
			if(targetParent)
			{
				const auto values = stableNodeList(destination.model->nodes[*targetParent].children, destination.plan);
				if(!values) { result.error = MutationErrorCode::staleTransaction; return result; }
				targetChildren = *values;
			}
			const auto composed = composeAnchored(currentChildren, sourceChildren,
				targetChildren, delta.m_impl->nodeMembershipOwned);
			if(!composed)
			{
				result.error = MutationErrorCode::staleTransaction;
				result.message = "owned child membership no longer composes";
				return result;
			}
			const auto numeric = numericNodeList(*composed, finalNodes);
			if(!numeric) { result.error = MutationErrorCode::staleTransaction; return result; }
			candidate->nodes[index].children = *numeric;
			const Plan& origin = nodeFromTarget[index] ? destination.plan : current;
			if(!normalizeNodeRecord(candidate->nodes[index], origin, plan))
			{
				result.error = MutationErrorCode::staleTransaction;
				result.message = "node references cannot be rebound to composed registries";
				return result;
			}
		}

		for(std::size_t index = 0; index < candidate->skins.size(); ++index)
			if(!normalizeSkinRecord(candidate->skins[index],
				skinFromTarget[index] ? destination.plan : current, plan))
			{
				result.error = MutationErrorCode::staleTransaction;
				return result;
			}
		for(std::size_t index = 0; index < candidate->animations.size(); ++index)
			if(!normalizeAnimationRecord(candidate->animations[index],
				animationFromTarget[index] ? destination.plan : current, plan))
			{
				result.error = MutationErrorCode::staleTransaction;
				return result;
			}

		const auto& currentSceneIds = current.idsByKind[kindIndex(EditorObjectKind::scene)];
		for(std::size_t sceneIndex = 0; sceneIndex < candidate->scenes.size(); ++sceneIndex)
		{
			const EditorObjectId scene = currentSceneIds[sceneIndex];
			const auto sourceScene = stableIndex(
				source.plan.idsByKind[kindIndex(EditorObjectKind::scene)], scene);
			const auto targetScene = stableIndex(
				destination.plan.idsByKind[kindIndex(EditorObjectKind::scene)], scene);
			std::vector<EditorObjectId> currentRoots, sourceRoots, targetRoots;
			if(const auto values = stableNodeList(currentModel->scenes[sceneIndex].nodes, current))
				currentRoots = *values;
			else { result.error = MutationErrorCode::staleTransaction; return result; }
			if(sourceScene)
			{
				const auto values = stableNodeList(source.model->scenes[*sourceScene].nodes, source.plan);
				if(!values) { result.error = MutationErrorCode::staleTransaction; return result; }
				sourceRoots = *values;
			}
			if(targetScene)
			{
				const auto values = stableNodeList(destination.model->scenes[*targetScene].nodes, destination.plan);
				if(!values) { result.error = MutationErrorCode::staleTransaction; return result; }
				targetRoots = *values;
			}
			const auto composed = composeAnchored(currentRoots, sourceRoots,
				targetRoots, delta.m_impl->nodeMembershipOwned);
			if(!composed) { result.error = MutationErrorCode::staleTransaction; return result; }
			const auto numeric = numericNodeList(*composed, finalNodes);
			if(!numeric) { result.error = MutationErrorCode::staleTransaction; return result; }
			candidate->scenes[sceneIndex].nodes = *numeric;
		}

		const auto& currentLightIds =
			current.idsByKind[kindIndex(EditorObjectKind::light)];
		const bool hasForeignPunctualLight = std::any_of(
			currentLightIds.begin(), currentLightIds.end(), [&](EditorObjectId id)
			{
				return !delta.m_impl->collectionOwned[
					kindIndex(EditorObjectKind::light)].contains(id);
			});
		const auto used = composeExtensionNames(currentModel->extensionsUsed,
			source.model->extensionsUsed, destination.model->extensionsUsed,
			delta.m_impl->extensionsUsedOwned,
			hasForeignPunctualLight ? std::string_view{"KHR_lights_punctual"}
				: std::string_view{});
		const auto required = composeExtensionNames(currentModel->extensionsRequired,
			source.model->extensionsRequired, destination.model->extensionsRequired,
			delta.m_impl->extensionsRequiredOwned);
		if(!used || !required)
		{
			result.error = MutationErrorCode::staleTransaction;
			result.message = "owned extension-name membership no longer composes";
			return result;
		}
		candidate->extensionsUsed = *used;
		candidate->extensionsRequired = *required;
		const bool retainsPunctualLights = !candidate->lights.empty()
			|| std::any_of(candidate->nodes.begin(), candidate->nodes.end(),
				[](const tinygltf::Node& node)
				{
					if(node.light >= 0) return true;
					const auto extension = node.extensions.find("KHR_lights_punctual");
					return extension != node.extensions.end()
						&& extension->second.IsObject()
						&& extension->second.Has("light");
				});
		if(retainsPunctualLights)
			ensureExtensionUsed(*candidate, "KHR_lights_punctual");
		plan.currentScene = m_document.currentSceneId();
		plan.sceneRootStates.clear();
		plan.frozenSceneRootIndicesByScene.assign(candidate->scenes.size(), {});
		for(const auto& targetRoot : destination.plan.sceneRootStates)
		{
			auto composedRoot = targetRoot;
			const auto expected = std::find_if(source.plan.sceneRootStates.begin(),
				source.plan.sceneRootStates.end(), [&](const auto& value)
				{
					return value.scene == targetRoot.scene;
				});
			if(expected != source.plan.sceneRootStates.end())
				composedRoot.expectedExternalStateSerial = expected->targetExternalStateSerial;
			composedRoot.preserveExternalOnMismatch = true;
			for(const EditorObjectId root : composedRoot.frozenRootIds)
			{
				const auto nodeIndex = stableIndex(finalNodes, root);
				const auto sceneIndex = stableIndex(currentSceneIds, targetRoot.scene);
				if(nodeIndex && sceneIndex)
					plan.frozenSceneRootIndicesByScene[*sceneIndex].push_back(*nodeIndex);
			}
			plan.sceneRootStates.push_back(std::move(composedRoot));
		}
		for(const EditorObjectId currentSceneId : currentSceneIds)
		{
			if(std::any_of(plan.sceneRootStates.begin(), plan.sceneRootStates.end(),
				[&](const auto& state) { return state.scene == currentSceneId; })) continue;
			const auto scene = SceneId::from(currentSceneId);
			const auto sceneIndex = stableIndex(currentSceneIds, currentSceneId);
			if(!scene || !sceneIndex)
			{
				result.error = MutationErrorCode::staleTransaction;
				return result;
			}
			auto currentRoot = rootStatePlan(*scene,
				m_document.sceneRootTransformState(*scene),
				m_document.frozenSceneRootNodeIds(*scene), true);
			for(const EditorObjectId root : currentRoot.frozenRootIds)
			{
				const auto nodeIndex = stableIndex(finalNodes, root);
				if(!nodeIndex)
				{
					result.error = MutationErrorCode::staleTransaction;
					return result;
				}
				plan.frozenSceneRootIndicesByScene[*sceneIndex].push_back(*nodeIndex);
			}
			plan.sceneRootStates.push_back(std::move(currentRoot));
		}
		plan.hierarchyChanged = true;
		plan.allowResurrection = true;
		plan.retainExternalStateSerial = true;
		const GltfValidationReport validation = detail::SceneCommandsAccess::validateModel(
			*candidate, m_limits);
		result.validation = validation;
		if(!validation.valid())
		{
			result.error = MutationErrorCode::validationFailed;
			result.message = "structural restore candidate failed validation";
			return result;
		}
		const std::uint64_t nextSerial = allocateMutationSerial();
		if(nextSerial == 0)
		{
			result.error = MutationErrorCode::internalError;
			result.message = "mutation serial space is exhausted";
			return result;
		}
		struct RestoreContext final
		{
			MutationResult* result;
			const Plan* before;
			const tinygltf::Model* beforeModel;
			const tinygltf::Model* afterModel;
			EditorDocumentAuthority authority;
			std::uint64_t baseRevision;
			std::uint64_t revision;
			std::uint64_t baseActivationEpoch;
			std::uint64_t serial;
		} context{&result, &current,
			&detail::SceneCommandsAccess::model(m_document), candidate.get(),
			m_document.documentAuthority(), m_document.revision(),
			nextRevisionWithoutWrap(m_document.revision()),
			m_document.sceneActivationEpoch(), nextSerial};
		const auto prePublish = [](const Plan& resolved, void* opaque)
		{
			auto& context = *static_cast<RestoreContext*>(opaque);
			context.result->status = MutationStatus::applied;
			context.result->error = MutationErrorCode::none;
			context.result->afterRevision = context.revision;
			context.result->transactionToken = MutationToken{
				context.authority, context.revision, context.serial};
			for(std::size_t kind = 1; kind < kindIndex(EditorObjectKind::count); ++kind)
			{
				addRemap(*context.result, context.before->idsByKind[kind],
					resolved.idsByKind[kind], static_cast<EditorObjectKind>(kind));
			}
			populateExactEvidence(*context.result, *context.beforeModel,
				*context.afterModel, *context.before, resolved, context.authority,
				context.baseRevision, context.revision, context.serial);
			if(context.before->currentScene != resolved.currentScene)
			{
				context.result->sceneActivationTransition = SceneActivationResult{
					SceneActivationStatus::Changed,
					context.before->currentScene.valid()
						? std::optional<SceneId>{context.before->currentScene} : std::nullopt,
					resolved.currentScene.valid()
						? std::optional<SceneId>{resolved.currentScene} : std::nullopt,
					context.baseActivationEpoch, context.baseActivationEpoch + 1u};
			}
		};
		result.message = target == StructuralState::undo
			? "structural delta restored to Undo" : "structural delta restored to Redo";
		detail::MutationCommitStatus status = detail::MutationCommitStatus::internalError;
		if(!detail::SceneCommandsAccess::commit(m_document, std::move(candidate),
			plan, prePublish, &context, status))
		{
			result.status = MutationStatus::rejected;
			result.afterRevision = result.beforeRevision;
			clearPublishedEvidence(result);
			result.transactionToken = token();
			result.error = commitStatusError(status);
			result.message = "structural restore failed before publication";
			return result;
		}
		m_serial = nextSerial;
		return result;
	}
	catch(const std::bad_alloc&)
	{
		result.error = MutationErrorCode::allocationFailed;
		result.message = "structural restore allocation failed";
		return result;
	}
	catch(...)
	{
		result.error = MutationErrorCode::internalError;
		result.message = "structural restore failed";
		return result;
	}
}

namespace {

[[nodiscard]] MutationErrorCode validateOccurrence(
	const GltfDocument& document,
	const SceneStructureSnapshot& snapshot,
	const SceneNodeOccurrence& occurrence,
	std::size_t& sceneIndex,
	std::size_t& nodeIndex,
	std::optional<std::size_t>& parentIndex) noexcept
{
	if(occurrence.scene.authority() != document.documentAuthority()
		|| occurrence.node.authority() != document.documentAuthority()
		|| (occurrence.parent
			&& occurrence.parent->authority() != document.documentAuthority()))
		return MutationErrorCode::crossDocument;
	if(occurrence.revision != document.revision()) return MutationErrorCode::staleOccurrence;
	const auto scene = document.indexOf(occurrence.scene.untyped());
	const auto node = document.indexOf(occurrence.node.untyped());
	if(!scene || !node) return MutationErrorCode::staleOccurrence;
	const auto currentParent = occurrenceParent(snapshot, *scene, *node);
	if(!currentParent) return MutationErrorCode::staleOccurrence;
	parentIndex.reset();
	if(*currentParent >= 0)
	{
		const auto parent = occurrence.parent
			? document.indexOf(occurrence.parent->untyped()) : std::nullopt;
		if(!parent || *parent != static_cast<std::size_t>(*currentParent))
			return MutationErrorCode::staleOccurrence;
		parentIndex = *parent;
	}
	else if(occurrence.parent)
	{
		return MutationErrorCode::staleOccurrence;
	}
	sceneIndex = *scene;
	nodeIndex = *node;
	return MutationErrorCode::none;
}

void rejectOccurrence(MutationResult& result, MutationErrorCode error)
{
	result.error = error;
	switch(error)
	{
		case MutationErrorCode::crossDocument:
			result.message = "occurrence belongs to another document";
			break;
		case MutationErrorCode::allocationFailed:
			result.message = "occurrence validation allocation failed";
			break;
		case MutationErrorCode::multiScenePolicy:
			result.message = "all-scenes source and target occurrence sets are inconsistent";
			break;
		case MutationErrorCode::cycle:
			result.message = "occurrence edit would create a node cycle";
			break;
		case MutationErrorCode::multiParent:
			result.message = "occurrence graph is structurally ambiguous or multiparent";
			break;
		case MutationErrorCode::limitExceeded:
			result.message = "occurrence traversal exceeds the configured bound";
			break;
		default:
			result.message = "occurrence is stale or structurally ambiguous";
			break;
	}
}

} // namespace

MutationResult SceneCommands::duplicateNodeAt(
	const SceneNodeOccurrence& source,
	std::optional<SceneNodeOccurrence> parent,
	SceneOccurrencePolicy policy,
	const MutationGuard& guard)
{
	MutationResult preflight = baseResult(
		HistoryPolicy::undoable, m_document, guard, m_serial);
	if(preflight.error != MutationErrorCode::none) return preflight;
	SceneStructureSnapshot snapshot;
	std::size_t sourceScene = 0u;
	std::size_t sourceIndex = 0u;
	std::optional<std::size_t> sourceParent;
	std::size_t targetScene = 0u;
	std::size_t targetIndex = 0u;
	std::optional<std::size_t> targetParent;
	MutationErrorCode error = MutationErrorCode::none;
	try
	{
		error = captureSceneStructure(
			detail::SceneCommandsAccess::model(m_document), m_limits, snapshot);
		if(error == MutationErrorCode::none)
			error = validateOccurrence(m_document, snapshot, source,
				sourceScene, sourceIndex, sourceParent);
		if(error == MutationErrorCode::none && parent)
		{
			error = validateOccurrence(m_document, snapshot, *parent,
				targetScene, targetIndex, targetParent);
			if(error == MutationErrorCode::none && targetScene != sourceScene)
				error = MutationErrorCode::multiScenePolicy;
		}
		if(error == MutationErrorCode::none && parent
			&& policy == SceneOccurrencePolicy::allScenes
			&& snapshot.scenesByNode[sourceIndex] != snapshot.scenesByNode[targetIndex])
			error = MutationErrorCode::multiScenePolicy;
	}
	catch(const std::bad_alloc&)
	{
		error = MutationErrorCode::allocationFailed;
	}
	catch(...)
	{
		error = MutationErrorCode::internalError;
	}
	if(error != MutationErrorCode::none)
	{
		rejectOccurrence(preflight, error);
		return preflight;
	}
	const bool isolateTarget = parent
		&& snapshot.scenesByNode[targetIndex].size() > 1u;
	return mutate(m_document, HistoryPolicy::undoable,
		guard, m_limits, m_faultInjector, m_serial,
		[&, snapshot = std::move(snapshot)](
			tinygltf::Model& model, Plan& plan, std::string&) mutable
	{
		plan.hierarchyChanged = true;
		if(policy == SceneOccurrencePolicy::clickedScene && isolateTarget)
		{
			const MutationErrorCode isolateError = isolateClickedScene(
				model, plan, snapshot, sourceScene, m_limits);
			if(isolateError != MutationErrorCode::none) return isolateError;
		}
		std::vector<std::size_t> subtree;
		MutationErrorCode mutationError = collectSubtree(
			snapshot, sourceIndex, m_limits, subtree);
		if(mutationError != MutationErrorCode::none) return mutationError;
		std::vector<int> cloneMap;
		mutationError = cloneNodes(model, plan, snapshot, subtree, m_limits, cloneMap);
		if(mutationError != MutationErrorCode::none) return mutationError;
		const int copyRoot = cloneMap[sourceIndex];
		if(copyRoot < 0) return MutationErrorCode::internalError;
		if(parent)
		{
			model.nodes[targetIndex].children.push_back(copyRoot);
		}
		else if(policy == SceneOccurrencePolicy::clickedScene)
		{
			model.scenes[sourceScene].nodes.push_back(copyRoot);
		}
		else
		{
			for(const std::size_t scene : snapshot.scenesByNode[sourceIndex])
				model.scenes[scene].nodes.push_back(copyRoot);
		}
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::deleteNodeAt(
	const SceneNodeOccurrence& occurrence,
	SceneOccurrencePolicy policy,
	const MutationGuard& guard)
{
	MutationResult preflight = baseResult(
		HistoryPolicy::undoable, m_document, guard, m_serial);
	if(preflight.error != MutationErrorCode::none) return preflight;
	SceneStructureSnapshot snapshot;
	std::size_t sceneIndex = 0u;
	std::size_t nodeIndex = 0u;
	std::optional<std::size_t> parentIndex;
	std::vector<std::size_t> subtree;
	MutationErrorCode error = MutationErrorCode::none;
	try
	{
		error = captureSceneStructure(
			detail::SceneCommandsAccess::model(m_document), m_limits, snapshot);
		if(error == MutationErrorCode::none)
			error = validateOccurrence(m_document, snapshot, occurrence,
				sceneIndex, nodeIndex, parentIndex);
		if(error == MutationErrorCode::none)
			error = collectSubtree(snapshot, nodeIndex, m_limits, subtree);
	}
	catch(const std::bad_alloc&)
	{
		error = MutationErrorCode::allocationFailed;
	}
	catch(...)
	{
		error = MutationErrorCode::internalError;
	}
	if(error != MutationErrorCode::none)
	{
		rejectOccurrence(preflight, error);
		return preflight;
	}
	const auto isUniqueSceneRoot = [&](std::size_t scene) noexcept
	{
		return scene < snapshot.sceneRoots.size()
			&& snapshot.sceneRoots[scene].size() == 1u
			&& snapshot.sceneRoots[scene].front() == static_cast<int>(nodeIndex);
	};
	bool uniqueRootNoOp = isUniqueSceneRoot(sceneIndex);
	if(policy == SceneOccurrencePolicy::allScenes)
	{
		uniqueRootNoOp = std::any_of(snapshot.scenesByNode[nodeIndex].begin(),
			snapshot.scenesByNode[nodeIndex].end(), isUniqueSceneRoot);
	}
	if(uniqueRootNoOp)
	{
		preflight.status = MutationStatus::domainNoop;
		preflight.error = MutationErrorCode::uniqueRootDeleteNoop;
		preflight.message = "deleting the unique scene root is a document no-op";
		return preflight;
	}
	const bool isolateSource = snapshot.scenesByNode[nodeIndex].size() > 1u;
	return mutate(m_document, HistoryPolicy::undoable,
		guard, m_limits, m_faultInjector, m_serial,
		[&, snapshot = std::move(snapshot), subtree = std::move(subtree)](
			tinygltf::Model& model, Plan& plan, std::string&) mutable
	{
		plan.hierarchyChanged = true;
		if(policy == SceneOccurrencePolicy::clickedScene && isolateSource)
		{
			const MutationErrorCode isolateError = isolateClickedScene(
				model, plan, snapshot, sceneIndex, m_limits);
			if(isolateError != MutationErrorCode::none) return isolateError;
		}
		std::vector<bool> deleted(model.nodes.size(), false);
		for(const std::size_t node : subtree)
			if(node < deleted.size()) deleted[node] = true;
		if(policy == SceneOccurrencePolicy::clickedScene)
		{
			if(parentIndex)
			{
				auto& children = model.nodes[*parentIndex].children;
				children.erase(std::remove(children.begin(), children.end(),
					static_cast<int>(nodeIndex)), children.end());
			}
			else
			{
				auto& roots = model.scenes[sceneIndex].nodes;
				roots.erase(std::remove(roots.begin(), roots.end(),
					static_cast<int>(nodeIndex)), roots.end());
			}
		}
		else
		{
			const int oldParent = snapshot.nodeParent[nodeIndex];
			if(oldParent >= 0)
			{
				auto& children = model.nodes[static_cast<std::size_t>(oldParent)].children;
				children.erase(std::remove(children.begin(), children.end(),
					static_cast<int>(nodeIndex)), children.end());
			}
			for(auto& scene : model.scenes)
			{
				scene.nodes.erase(std::remove(scene.nodes.begin(), scene.nodes.end(),
					static_cast<int>(nodeIndex)), scene.nodes.end());
			}
		}
		std::vector<bool> reachable;
		const MutationErrorCode reachabilityError = markSceneReachableNodes(
			model, m_limits, reachable);
		if(reachabilityError != MutationErrorCode::none) return reachabilityError;
		for(std::size_t node = 0; node < deleted.size(); ++node)
			deleted[node] = deleted[node] && !reachable[node];
		eraseDeletedNodes(model, plan, deleted);
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::reparentNodeAt(
	const SceneNodeOccurrence& source,
	std::optional<SceneNodeOccurrence> parent,
	SceneOccurrencePolicy policy,
	const MutationGuard& guard)
{
	return reparentNodeAt(source, std::move(parent), policy, SceneId{}, guard);
}

MutationResult SceneCommands::reparentNodeAt(
	const SceneNodeOccurrence& source,
	std::optional<SceneNodeOccurrence> parent,
	SceneOccurrencePolicy policy,
	SceneId targetSceneId,
	const MutationGuard& guard)
{
	MutationResult preflight = baseResult(
		HistoryPolicy::undoable, m_document, guard, m_serial);
	if(preflight.error != MutationErrorCode::none) return preflight;
	const SceneId requestedTargetScene = targetSceneId.valid()
		? targetSceneId
		: parent ? parent->scene : source.scene;
	SceneStructureSnapshot snapshot;
	std::size_t sourceScene = 0u;
	std::size_t sourceIndex = 0u;
	std::optional<std::size_t> sourceParent;
	std::size_t targetScene = 0u;
	std::size_t targetIndex = 0u;
	std::optional<std::size_t> targetParent;
	MutationErrorCode error = MutationErrorCode::none;
	try
	{
		error = captureSceneStructure(
			detail::SceneCommandsAccess::model(m_document), m_limits, snapshot);
		if(error == MutationErrorCode::none)
			error = validateOccurrence(m_document, snapshot, source,
				sourceScene, sourceIndex, sourceParent);
		if(error == MutationErrorCode::none)
		{
			if(requestedTargetScene.authority() != m_document.documentAuthority())
				error = MutationErrorCode::crossDocument;
			else
			{
				const auto targetIndexValue = m_document.indexOf(
					requestedTargetScene.untyped());
				if(!targetIndexValue)
					error = MutationErrorCode::staleOccurrence;
				else
					targetScene = *targetIndexValue;
			}
		}
		if(error == MutationErrorCode::none && parent)
		{
			std::size_t parentScene = 0u;
			error = validateOccurrence(m_document, snapshot, *parent,
				parentScene, targetIndex, targetParent);
			if(error == MutationErrorCode::none && targetScene != sourceScene)
				error = MutationErrorCode::multiScenePolicy;
			if(error == MutationErrorCode::none && parentScene != targetScene)
				error = MutationErrorCode::multiScenePolicy;
		}
		if(error == MutationErrorCode::none &&
			policy == SceneOccurrencePolicy::allScenes &&
			targetScene != sourceScene)
			error = MutationErrorCode::multiScenePolicy;
		if(error == MutationErrorCode::none && parent)
		{
			bool cyclic = false;
			error = boundedReachable(snapshot, sourceIndex, targetIndex, m_limits, cyclic);
			if(error == MutationErrorCode::none && cyclic) error = MutationErrorCode::cycle;
		}
		if(error == MutationErrorCode::none && parent
			&& policy == SceneOccurrencePolicy::allScenes
			&& snapshot.scenesByNode[sourceIndex] != snapshot.scenesByNode[targetIndex])
			error = MutationErrorCode::multiScenePolicy;
	}
	catch(const std::bad_alloc&)
	{
		error = MutationErrorCode::allocationFailed;
	}
	catch(...)
	{
		error = MutationErrorCode::internalError;
	}
	if(error != MutationErrorCode::none)
	{
		rejectOccurrence(preflight, error);
		return preflight;
	}
	bool noOp = false;
	if(policy == SceneOccurrencePolicy::clickedScene)
	{
		noOp = parent ? (sourceParent && *sourceParent == targetIndex)
			: (targetScene == sourceScene && !sourceParent);
	}
	else if(parent)
	{
		noOp = snapshot.nodeParent[sourceIndex] == static_cast<int>(targetIndex);
		for(const std::size_t scene : snapshot.scenesByNode[sourceIndex])
		{
			const auto directParent = occurrenceParent(snapshot, scene, sourceIndex);
			noOp = noOp && directParent && *directParent == static_cast<int>(targetIndex);
		}
	}
	else
	{
		noOp = snapshot.nodeParent[sourceIndex] < 0;
		for(const std::size_t scene : snapshot.scenesByNode[sourceIndex])
		{
			const auto directParent = occurrenceParent(snapshot, scene, sourceIndex);
			noOp = noOp && directParent && *directParent < 0;
		}
	}
	if(noOp)
	{
		preflight.status = MutationStatus::domainNoop;
		preflight.error = MutationErrorCode::noOp;
		preflight.message = "operation has no document effect";
		return preflight;
	}
	const bool isolateShared = snapshot.scenesByNode[sourceIndex].size() > 1u
		|| (parent && snapshot.scenesByNode[targetIndex].size() > 1u);
	return mutate(m_document, HistoryPolicy::undoable,
		guard, m_limits, m_faultInjector, m_serial,
		[&, snapshot = std::move(snapshot)](
			tinygltf::Model& model, Plan& plan, std::string&) mutable
	{
		plan.hierarchyChanged = true;
		if(policy == SceneOccurrencePolicy::clickedScene)
		{
			if(isolateShared)
			{
				const MutationErrorCode isolateError = isolateClickedScene(
					model, plan, snapshot, sourceScene, m_limits);
				if(isolateError != MutationErrorCode::none) return isolateError;
			}
			if(sourceParent)
			{
				auto& children = model.nodes[*sourceParent].children;
				children.erase(std::remove(children.begin(), children.end(),
					static_cast<int>(sourceIndex)), children.end());
			}
			else
			{
				auto& roots = model.scenes[sourceScene].nodes;
				roots.erase(std::remove(roots.begin(), roots.end(),
					static_cast<int>(sourceIndex)), roots.end());
			}
			if(parent)
				model.nodes[targetIndex].children.push_back(static_cast<int>(sourceIndex));
			else
				model.scenes[targetScene].nodes.push_back(static_cast<int>(sourceIndex));
		}
		else
		{
			const int oldParent = snapshot.nodeParent[sourceIndex];
			if(oldParent >= 0)
			{
				auto& children = model.nodes[static_cast<std::size_t>(oldParent)].children;
				children.erase(std::remove(children.begin(), children.end(),
					static_cast<int>(sourceIndex)), children.end());
			}
			for(auto& scene : model.scenes)
			{
				scene.nodes.erase(std::remove(scene.nodes.begin(), scene.nodes.end(),
					static_cast<int>(sourceIndex)), scene.nodes.end());
			}
			if(parent)
				model.nodes[targetIndex].children.push_back(static_cast<int>(sourceIndex));
			else
				for(const std::size_t scene : snapshot.scenesByNode[sourceIndex])
					model.scenes[scene].nodes.push_back(static_cast<int>(sourceIndex));
		}
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::addScene(std::string name, const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::nonHistory,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan& plan, std::string& message)
	{
		plan.hierarchyChanged = true;
		if(model.scenes.size() >= m_limits.maxScenes) return MutationErrorCode::limitExceeded;
		tinygltf::Scene scene;
		scene.name = std::move(name);
		model.scenes.push_back(std::move(scene));
		plan.idsByKind[kindIndex(EditorObjectKind::scene)].push_back(EditorObjectId{});
		if(model.defaultScene < 0) model.defaultScene = static_cast<int>(model.scenes.size() - 1u);
		plan.hierarchyChanged = true;
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::deleteScene(SceneId scene, const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::nonHistory,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan& plan, std::string& message)
	{
		plan.hierarchyChanged = true;
		const auto index = m_document.indexOf(scene.untyped());
		if(!index) return scene.authority() != m_document.documentAuthority()
			? MutationErrorCode::crossDocument : MutationErrorCode::staleId;
		if(model.scenes.size() <= 1u) return MutationErrorCode::lastScene;
		const std::size_t deleted = *index;
		model.scenes.erase(model.scenes.begin() + static_cast<std::ptrdiff_t>(deleted));
		plan.idsByKind[kindIndex(EditorObjectKind::scene)].erase(
			plan.idsByKind[kindIndex(EditorObjectKind::scene)].begin() + static_cast<std::ptrdiff_t>(deleted));
		if(model.defaultScene == static_cast<int>(deleted)) model.defaultScene = 0;
		else if(model.defaultScene > static_cast<int>(deleted)) --model.defaultScene;
		if(m_document.currentSceneId() == scene) plan.currentScene = {};
		message = "scene deleted";
		plan.hierarchyChanged = true;
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::addNode(SceneId scene, std::string name,
	std::optional<NodeId> parent, const MutationGuard& guard)
{
	const HistoryPolicy historyPolicy = parent
		? HistoryPolicy::undoable : HistoryPolicy::nonHistory;
	return mutate(m_document, historyPolicy,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan& plan, std::string& message)
	{
		plan.hierarchyChanged = true;
		const auto sceneIndex = m_document.indexOf(scene.untyped());
		if(!sceneIndex) return scene.authority() != m_document.documentAuthority()
			? MutationErrorCode::crossDocument : MutationErrorCode::staleId;
		if(model.nodes.size() >= m_limits.maxNodes) return MutationErrorCode::limitExceeded;
		int parentIndex = -1;
		if(parent)
		{
			const auto parentValue = m_document.indexOf(parent->untyped());
			if(!parentValue) return parent->authority() != m_document.documentAuthority()
				? MutationErrorCode::crossDocument : MutationErrorCode::staleId;
			parentIndex = static_cast<int>(*parentValue);
			const auto parentScenes = occurrences(model, parentIndex);
			if(std::find(parentScenes.begin(), parentScenes.end(), *sceneIndex) == parentScenes.end())
				return MutationErrorCode::multiScenePolicy;
		}
		tinygltf::Node node;
		node.name = std::move(name);
		const int newIndex = static_cast<int>(model.nodes.size());
		model.nodes.push_back(std::move(node));
		plan.idsByKind[kindIndex(EditorObjectKind::node)].push_back(EditorObjectId{});
		if(parentIndex >= 0) model.nodes[static_cast<std::size_t>(parentIndex)].children.push_back(newIndex);
		else model.scenes[*sceneIndex].nodes.push_back(newIndex);
		plan.hierarchyChanged = true;
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::duplicateNode(NodeId node, std::optional<NodeId> parent,
	const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::undoable,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan& plan, std::string& message)
	{
		plan.hierarchyChanged = true;
		const auto source = m_document.indexOf(node.untyped());
		if(!source) return node.authority() != m_document.documentAuthority()
			? MutationErrorCode::crossDocument : MutationErrorCode::staleId;
		const auto sceneHits = occurrences(model, static_cast<int>(*source));
		if(sceneHits.size() > 1u) return MutationErrorCode::multiScenePolicy;
		std::unordered_map<int, int> cloned;
		std::vector<int> cloneOrder;
		std::vector<int> stack{static_cast<int>(*source)};
		while(!stack.empty())
		{
			const int current = stack.back(); stack.pop_back();
			if(cloned.contains(current)) continue;
			if(model.nodes.size() + cloned.size() >= m_limits.maxNodes) return MutationErrorCode::limitExceeded;
			const int copyIndex = static_cast<int>(model.nodes.size() + cloned.size());
			cloned.emplace(current, copyIndex);
			cloneOrder.push_back(current);
			for(const int child : model.nodes[static_cast<std::size_t>(current)].children) stack.push_back(child);
		}
		const std::size_t originalSize = model.nodes.size();
		std::unordered_map<int, int> clonedSkins;
		for(const int oldIndex : cloneOrder)
		{
			const int skinIndex = model.nodes[static_cast<std::size_t>(oldIndex)].skin;
			if(skinIndex < 0 || clonedSkins.contains(skinIndex)) continue;
			if(static_cast<std::size_t>(skinIndex) >= model.skins.size())
				return MutationErrorCode::invalidReference;
			if(model.skins.size() + clonedSkins.size() >= m_limits.maxSkins)
				return MutationErrorCode::limitExceeded;
			clonedSkins.emplace(skinIndex,
				static_cast<int>(model.skins.size() + clonedSkins.size()));
		}
		model.skins.reserve(model.skins.size() + clonedSkins.size());
		std::vector<std::pair<int, int>> skinOrder(clonedSkins.begin(), clonedSkins.end());
		std::sort(skinOrder.begin(), skinOrder.end());
		for(std::size_t index = 0; index < skinOrder.size(); ++index)
		{
			skinOrder[index].second = static_cast<int>(model.skins.size() + index);
			clonedSkins[skinOrder[index].first] = skinOrder[index].second;
		}
		for(const auto [oldSkin, newSkin] : skinOrder)
		{
			(void)newSkin;
			tinygltf::Skin copy = model.skins[static_cast<std::size_t>(oldSkin)];
			for(int& joint : copy.joints)
			{
				const auto mapped = cloned.find(joint);
				if(mapped != cloned.end()) joint = mapped->second;
			}
			const auto skeleton = cloned.find(copy.skeleton);
			if(skeleton != cloned.end()) copy.skeleton = skeleton->second;
			model.skins.push_back(std::move(copy));
			plan.idsByKind[kindIndex(EditorObjectKind::skin)].push_back(EditorObjectId{});
		}
		model.nodes.reserve(originalSize + cloned.size());
		for(const int oldIndex : cloneOrder)
		{
			tinygltf::Node copy = model.nodes[static_cast<std::size_t>(oldIndex)];
			for(int& child : copy.children) child = cloned.at(child);
			const auto skin = clonedSkins.find(copy.skin);
			if(skin != clonedSkins.end()) copy.skin = skin->second;
			model.nodes.push_back(std::move(copy));
		}
		appendInvalid<tinygltf::Node>(plan.idsByKind[kindIndex(EditorObjectKind::node)], cloned.size());
		std::size_t totalChannels = 0;
		for(const auto& animation : model.animations) totalChannels += animation.channels.size();
		for(auto& animation : model.animations)
		{
			const std::size_t originalChannels = animation.channels.size();
			for(std::size_t channelIndex = 0; channelIndex < originalChannels; ++channelIndex)
			{
				const auto mapped = cloned.find(animation.channels[channelIndex].target_node);
				if(mapped == cloned.end()) continue;
				if(totalChannels >= m_limits.maxAnimationChannels)
					return MutationErrorCode::limitExceeded;
				tinygltf::AnimationChannel copy = animation.channels[channelIndex];
				copy.target_node = mapped->second;
				animation.channels.push_back(std::move(copy));
				++totalChannels;
			}
		}
		int targetParent = -1;
		if(parent)
		{
			const auto target = m_document.indexOf(parent->untyped());
			if(!target) return parent->authority() != m_document.documentAuthority()
				? MutationErrorCode::crossDocument : MutationErrorCode::staleId;
			targetParent = static_cast<int>(*target);
			if(reachable(model, targetParent, static_cast<int>(*source))) return MutationErrorCode::cycle;
		}
		const int copyRoot = cloned.at(static_cast<int>(*source));
		if(targetParent >= 0) model.nodes[static_cast<std::size_t>(targetParent)].children.push_back(copyRoot);
		else if(!sceneHits.empty()) model.scenes[sceneHits.front()].nodes.push_back(copyRoot);
		else return MutationErrorCode::multiScenePolicy;
		plan.hierarchyChanged = true;
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::deleteNode(NodeId node, const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::undoable,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan& plan, std::string& message)
	{
		plan.hierarchyChanged = true;
		const auto index = m_document.indexOf(node.untyped());
		if(!index) return node.authority() != m_document.documentAuthority()
			? MutationErrorCode::crossDocument : MutationErrorCode::staleId;
		const int root = static_cast<int>(*index);
		const auto sceneHits = occurrences(model, root);
		if(sceneHits.size() > 1u) return MutationErrorCode::multiScenePolicy;
		std::vector<bool> deleted(model.nodes.size(), false);
		markSubtree(model, root, deleted);
		if(!sceneHits.empty() && model.scenes[sceneHits.front()].nodes.size() == 1u
			&& model.scenes[sceneHits.front()].nodes.front() == root)
		{
			return MutationErrorCode::uniqueRootDeleteNoop;
		}
		for(std::size_t old = 0; old < model.nodes.size(); ++old)
		{
			if(deleted[old] && nodeParentCount(model, static_cast<int>(old)) > 1u) return MutationErrorCode::multiParent;
		}
		std::vector<int> map(model.nodes.size(), -1);
		int next = 0;
		for(std::size_t old = 0; old < model.nodes.size(); ++old) if(!deleted[old]) map[old] = next++;
		std::vector<tinygltf::Node> nodes;
		nodes.reserve(static_cast<std::size_t>(next));
		for(std::size_t old = 0; old < model.nodes.size(); ++old)
		{
			if(deleted[old]) continue;
			tinygltf::Node copy = model.nodes[old];
			remapIndices(copy.children, map);
			nodes.push_back(std::move(copy));
		}
		for(auto& scene : model.scenes) remapIndices(scene.nodes, map);
		std::vector<int> skinMap(model.skins.size(), -1);
		std::vector<tinygltf::Skin> skins;
		std::vector<EditorObjectId> skinIds;
		skins.reserve(model.skins.size());
		skinIds.reserve(model.skins.size());
		const auto oldSkinIds = plan.idsByKind[kindIndex(EditorObjectKind::skin)];
		for(std::size_t old = 0; old < model.skins.size(); ++old)
		{
			tinygltf::Skin copy = model.skins[old];
			remapIndices(copy.joints, map);
			remapIndex(copy.skeleton, map);
			if(copy.joints.empty()) continue;
			skinMap[old] = static_cast<int>(skins.size());
			skins.push_back(std::move(copy));
			skinIds.push_back(oldSkinIds[old]);
		}
		for(auto& nodeValue : nodes) remapIndex(nodeValue.skin, skinMap);
		model.skins.swap(skins);
		plan.idsByKind[kindIndex(EditorObjectKind::skin)] = std::move(skinIds);

		std::vector<tinygltf::Animation> animations;
		std::vector<EditorObjectId> animationIds;
		animations.reserve(model.animations.size());
		animationIds.reserve(model.animations.size());
		const auto oldAnimationIds = plan.idsByKind[kindIndex(EditorObjectKind::animation)];
		for(std::size_t old = 0; old < model.animations.size(); ++old)
		{
			tinygltf::Animation copy = model.animations[old];
			copy.channels.erase(std::remove_if(copy.channels.begin(), copy.channels.end(),
				[&](const tinygltf::AnimationChannel& channel)
				{
					return channel.target_node >= 0
						&& static_cast<std::size_t>(channel.target_node) < deleted.size()
						&& deleted[static_cast<std::size_t>(channel.target_node)];
				}), copy.channels.end());
			for(auto& channel : copy.channels) remapIndex(channel.target_node, map);
			if(copy.channels.empty()) continue;
			animations.push_back(std::move(copy));
			animationIds.push_back(oldAnimationIds[old]);
		}
		model.animations.swap(animations);
		plan.idsByKind[kindIndex(EditorObjectKind::animation)] = std::move(animationIds);
		model.nodes.swap(nodes);
		const auto oldNodeIds = plan.idsByKind[kindIndex(EditorObjectKind::node)];
		std::vector<EditorObjectId> nextNodeIds;
		nextNodeIds.reserve(model.nodes.size());
		for(std::size_t old = 0; old < deleted.size(); ++old)
			if(!deleted[old]) nextNodeIds.push_back(oldNodeIds[old]);
		plan.idsByKind[kindIndex(EditorObjectKind::node)] = std::move(nextNodeIds);
		if(model.defaultScene >= static_cast<int>(model.scenes.size())) model.defaultScene = model.scenes.empty() ? -1 : 0;
		plan.hierarchyChanged = true;
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::reparentNode(NodeId node, std::optional<NodeId> parent,
	const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::undoable,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan& plan, std::string& message)
	{
		plan.hierarchyChanged = true;
		const auto source = m_document.indexOf(node.untyped());
		if(!source) return node.authority() != m_document.documentAuthority()
			? MutationErrorCode::crossDocument : MutationErrorCode::staleId;
		const int sourceIndex = static_cast<int>(*source);
		const auto sourceScenes = occurrences(model, sourceIndex);
		int oldParent = -1;
		const bool hasParent = hasNodeParent(model, sourceIndex, &oldParent);
		if(hasParent && nodeParentCount(model, sourceIndex) > 1u)
		{
			return MutationErrorCode::multiParent;
		}
		int targetParent = -1;
		if(parent)
		{
			const auto target = m_document.indexOf(parent->untyped());
			if(!target) return parent->authority() != m_document.documentAuthority()
				? MutationErrorCode::crossDocument : MutationErrorCode::staleId;
			targetParent = static_cast<int>(*target);
			if(targetParent == sourceIndex || reachable(model, sourceIndex, targetParent)) return MutationErrorCode::cycle;
			if(occurrences(model, targetParent).size() != 1u) return MutationErrorCode::multiScenePolicy;
		}
		const bool isRootInAllOccurrences = !hasParent && targetParent < 0
			&& !sourceScenes.empty()
			&& std::all_of(sourceScenes.begin(), sourceScenes.end(), [&](std::size_t sceneIndex)
			{
				const auto& roots = model.scenes[sceneIndex].nodes;
				return std::count(roots.begin(), roots.end(), sourceIndex) == 1;
			});
		if(oldParent == targetParent && (sourceScenes.size() == 1u || isRootInAllOccurrences))
			return MutationErrorCode::noOp;
		if(sourceScenes.size() != 1u) return MutationErrorCode::multiScenePolicy;
		if(oldParent >= 0)
		{
			auto& children = model.nodes[static_cast<std::size_t>(oldParent)].children;
			children.erase(std::remove(children.begin(), children.end(), sourceIndex), children.end());
		}
		else
		{
			auto& roots = model.scenes[sourceScenes.front()].nodes;
			roots.erase(std::remove(roots.begin(), roots.end(), sourceIndex), roots.end());
		}
		if(targetParent >= 0) model.nodes[static_cast<std::size_t>(targetParent)].children.push_back(sourceIndex);
		else model.scenes[sourceScenes.front()].nodes.push_back(sourceIndex);
		plan.hierarchyChanged = true;
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::rename(EditorObjectId id, std::string name, const MutationGuard& guard)
{
	const HistoryPolicy historyPolicy = id.kind == EditorObjectKind::node
		? HistoryPolicy::undoable : HistoryPolicy::nonHistory;
	if(id.kind == EditorObjectKind::node)
	{
		const auto node = NodeId::from(id);
		if(node)
			return mutateNodeNameOnly(m_document, *node, std::move(name), guard,
				m_limits, m_faultInjector, m_serial);
	}
	// Every non-node rename keeps the general candidate/validation path.  It
	// may alter topology-adjacent metadata and must not reuse a node digest hint.
	return mutate(m_document, historyPolicy,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan&, std::string& message)
	{
		if(id.authority != m_document.documentAuthority()) return MutationErrorCode::crossDocument;
		const auto index = m_document.indexOf(id);
		if(!index) return MutationErrorCode::staleId;
		if(name.size() > m_limits.maxStringBytes) return MutationErrorCode::limitExceeded;
		if(id.kind == EditorObjectKind::primitive) return MutationErrorCode::primitiveHasNoName;
		std::string* target = nullptr;
		switch(id.kind)
		{
			case EditorObjectKind::scene: target = &model.scenes[*index].name; break;
			case EditorObjectKind::node: target = &model.nodes[*index].name; break;
			case EditorObjectKind::mesh: target = &model.meshes[*index].name; break;
			case EditorObjectKind::material: target = &model.materials[*index].name; break;
			case EditorObjectKind::texture: target = &model.textures[*index].name; break;
			case EditorObjectKind::image: target = &model.images[*index].name; break;
			case EditorObjectKind::sampler: target = &model.samplers[*index].name; break;
			case EditorObjectKind::camera: target = &model.cameras[*index].name; break;
			case EditorObjectKind::light: target = &model.lights[*index].name; break;
			case EditorObjectKind::accessor: target = &model.accessors[*index].name; break;
			case EditorObjectKind::bufferView: target = &model.bufferViews[*index].name; break;
			case EditorObjectKind::buffer: target = &model.buffers[*index].name; break;
			case EditorObjectKind::skin: target = &model.skins[*index].name; break;
			case EditorObjectKind::animation: target = &model.animations[*index].name; break;
			default: return MutationErrorCode::unsupportedKind;
		}
		if(*target == name) return MutationErrorCode::noOp;
		*target = std::move(name);
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::setNodeTransform(NodeId node, const NodeTransform& transform,
	const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::undoable,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan&, std::string& message)
	{
		if(node.authority() != m_document.documentAuthority()) return MutationErrorCode::crossDocument;
		const auto index = m_document.indexOf(node.untyped());
		if(!index) return MutationErrorCode::staleId;
		if(!finite(transform.translation) || !finite(transform.rotation) || !finite(transform.scale))
			return MutationErrorCode::invalidTransform;
		const double norm = std::sqrt(std::inner_product(transform.rotation.begin(), transform.rotation.end(),
			transform.rotation.begin(), 0.0));
		if(!std::isfinite(norm) || std::abs(norm - 1.0) > 1e-4) return MutationErrorCode::invalidTransform;
		if(sameTransform(model.nodes[*index], transform)) return MutationErrorCode::noOp;
		auto& target = model.nodes[*index];
		target.matrix.clear();
		target.translation.assign(transform.translation.begin(), transform.translation.end());
		target.rotation.assign(transform.rotation.begin(), transform.rotation.end());
		target.scale.assign(transform.scale.begin(), transform.scale.end());
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::attachMesh(NodeId node, MeshId mesh, const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::nonHistory,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan&, std::string&)
	{
		const auto nodeIndex = m_document.indexOf(node.untyped());
		const auto meshIndex = m_document.indexOf(mesh.untyped());
		if(node.authority() != m_document.documentAuthority() || mesh.authority() != m_document.documentAuthority()) return MutationErrorCode::crossDocument;
		if(!nodeIndex || !meshIndex) return MutationErrorCode::staleId;
		if(model.nodes[*nodeIndex].mesh == static_cast<int>(*meshIndex)) return MutationErrorCode::noOp;
		model.nodes[*nodeIndex].mesh = static_cast<int>(*meshIndex);
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::attachCamera(NodeId node, std::optional<CameraId> camera, const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::nonHistory,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan&, std::string&)
	{
		const auto nodeIndex = m_document.indexOf(node.untyped());
		if(!nodeIndex) return node.authority() != m_document.documentAuthority() ? MutationErrorCode::crossDocument : MutationErrorCode::staleId;
		int cameraIndex = -1;
		if(camera)
		{
			const auto index = m_document.indexOf(camera->untyped());
			if(!index) return camera->authority() != m_document.documentAuthority() ? MutationErrorCode::crossDocument : MutationErrorCode::staleId;
			cameraIndex = static_cast<int>(*index);
		}
		if(model.nodes[*nodeIndex].camera == cameraIndex) return MutationErrorCode::noOp;
		model.nodes[*nodeIndex].camera = cameraIndex;
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::attachLight(NodeId node, std::optional<LightId> light, const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::nonHistory,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan&, std::string&)
	{
		const auto nodeIndex = m_document.indexOf(node.untyped());
		if(!nodeIndex) return node.authority() != m_document.documentAuthority() ? MutationErrorCode::crossDocument : MutationErrorCode::staleId;
		int lightIndex = -1;
		if(light)
		{
			const auto index = m_document.indexOf(light->untyped());
			if(!index) return light->authority() != m_document.documentAuthority() ? MutationErrorCode::crossDocument : MutationErrorCode::staleId;
			lightIndex = static_cast<int>(*index);
		}
		if(model.nodes[*nodeIndex].light == lightIndex) return MutationErrorCode::noOp;
		model.nodes[*nodeIndex].light = lightIndex;
		if(lightIndex >= 0) ensureExtensionUsed(model, "KHR_lights_punctual");
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::setVisibility(EditorObjectId id, bool visible, const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::nonHistory,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan&, std::string& message)
	{
		if(id.authority != m_document.documentAuthority())
			return MutationErrorCode::crossDocument;
		if(id.kind != EditorObjectKind::node)
			return MutationErrorCode::unsupportedKind;
		const auto index = m_document.indexOf(id);
		if(!index) return MutationErrorCode::staleId;
		const auto existing = model.nodes[*index].extensions.find("KHR_node_visibility");
		if(existing != model.nodes[*index].extensions.end() && existing->second.IsObject()
			&& existing->second.Has("visible") && existing->second.Get("visible").IsBool()
			&& existing->second.Get("visible").Get<bool>() == visible)
		{
			return MutationErrorCode::noOp;
		}
		tinygltf::Value::Object authored;
		authored.emplace("visible", tinygltf::Value(visible));
		model.nodes[*index].extensions["KHR_node_visibility"] =
			tinygltf::Value(std::move(authored));
		ensureExtensionUsed(model, "KHR_node_visibility");
		message = "node visibility extension authored";
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::removeVisibility(NodeId node, const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::nonHistory,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan&, std::string& message)
	{
		if(node.authority() != m_document.documentAuthority())
			return MutationErrorCode::crossDocument;
		const auto index = m_document.indexOf(node.untyped());
		if(!index) return MutationErrorCode::staleId;
		auto& extensions = model.nodes[*index].extensions;
		if(!extensions.erase("KHR_node_visibility")) return MutationErrorCode::noOp;
		removeNodeExtensionUsedIfUnreferenced(model, "KHR_node_visibility");
		message = "node visibility extension removed";
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::setMaterial(MaterialId material, const MaterialProperties& properties,
	const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::undoable,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan&, std::string&)
	{
		const auto index = m_document.indexOf(material.untyped());
		if(!index) return material.authority() != m_document.documentAuthority() ? MutationErrorCode::crossDocument : MutationErrorCode::staleId;
		if(!validMaterialProperties(properties)) return MutationErrorCode::invalidReference;
		auto& target = model.materials[*index];
		if(materialPropertiesEqual(target, properties)) return MutationErrorCode::noOp;
		target.pbrMetallicRoughness.baseColorFactor.assign(properties.baseColorFactor.begin(), properties.baseColorFactor.end());
		target.pbrMetallicRoughness.metallicFactor = properties.metallicFactor;
		target.pbrMetallicRoughness.roughnessFactor = properties.roughnessFactor;
		target.emissiveFactor.assign(properties.emissiveFactor.begin(), properties.emissiveFactor.end());
		target.alphaCutoff = properties.alphaCutoff;
		target.alphaMode = properties.alphaMode;
		target.doubleSided = properties.doubleSided;
		return MutationErrorCode::none;
	});
}

std::optional<NodeTransform> SceneCommands::nodeTransform(NodeId node) const noexcept
{
	const auto index = m_document.indexOf(node.untyped());
	if(!index || node.authority() != m_document.documentAuthority()) return std::nullopt;
	const tinygltf::Node& source = detail::SceneCommandsAccess::model(m_document).nodes[*index];
	if(!source.matrix.empty()) return std::nullopt;
	if((!source.translation.empty() && source.translation.size() != 3u)
		|| (!source.rotation.empty() && source.rotation.size() != 4u)
		|| (!source.scale.empty() && source.scale.size() != 3u)) return std::nullopt;
	NodeTransform result;
	if(!source.translation.empty()) std::copy_n(source.translation.begin(), 3, result.translation.begin());
	if(!source.rotation.empty()) std::copy_n(source.rotation.begin(), 4, result.rotation.begin());
	if(!source.scale.empty()) std::copy_n(source.scale.begin(), 3, result.scale.begin());
	return result;
}

std::optional<MaterialSnapshot> SceneCommands::materialSnapshot(MaterialId material) const
{
	const auto index = m_document.indexOf(material.untyped());
	if(!index || material.authority() != m_document.documentAuthority()) return std::nullopt;
	try
	{
		auto impl = std::make_shared<MaterialSnapshot::Impl>();
		impl->id = material;
		impl->authority = m_document.documentAuthority();
		impl->record = detail::SceneCommandsAccess::model(m_document).materials[*index];
		if(!collectMaterialSnapshotReferences(
			m_document, impl->record, impl->stableReferences)) return std::nullopt;
		const auto bytes = materialSnapshotBytes(impl->record, impl->stableReferences);
		if(!bytes) return std::nullopt;
		impl->bytes = *bytes;
		MaterialSnapshot result;
		result.m_impl = std::move(impl);
		return result;
	}
	catch(...)
	{
		return std::nullopt;
	}
}

std::optional<LightSnapshot> SceneCommands::lightSnapshot(LightId light) const
{
	const auto index = m_document.indexOf(light.untyped());
	if(!index || light.authority() != m_document.documentAuthority()) return std::nullopt;
	try
	{
		auto impl = std::make_shared<LightSnapshot::Impl>();
		impl->id = light;
		impl->authority = m_document.documentAuthority();
		impl->record = detail::SceneCommandsAccess::model(m_document).lights[*index];
		const auto bytes = lightRecordBytes(impl->record);
		if(!bytes) return std::nullopt;
		impl->bytes = *bytes;
		LightSnapshot result;
		result.m_impl = std::move(impl);
		return result;
	}
	catch(...)
	{
		return std::nullopt;
	}
}

std::optional<MaterialSnapshot> SceneCommands::prepareMaterialSnapshot(
	MaterialId material, const MaterialEdit& edit) const
{
	auto current = materialSnapshot(material);
	if(!current) return std::nullopt;
	try
	{
		auto impl = std::make_shared<MaterialSnapshot::Impl>(*current->m_impl);
		tinygltf::Material& target = impl->record;
		bool valid = true;
		std::visit([&](const auto& value)
		{
			using T = std::decay_t<decltype(value)>;
			if constexpr(std::is_same_v<T, MaterialProperties>)
			{
				valid = validMaterialProperties(value);
				if(!valid) return;
				target.pbrMetallicRoughness.baseColorFactor.assign(
					value.baseColorFactor.begin(), value.baseColorFactor.end());
				target.pbrMetallicRoughness.metallicFactor = value.metallicFactor;
				target.pbrMetallicRoughness.roughnessFactor = value.roughnessFactor;
				target.emissiveFactor.assign(value.emissiveFactor.begin(), value.emissiveFactor.end());
				target.alphaCutoff = value.alphaCutoff;
				target.alphaMode = value.alphaMode;
				target.doubleSided = value.doubleSided;
			}
			else if constexpr(std::is_same_v<T, MaterialDraftProperties>)
			{
				valid = validMaterialProperties(value.properties);
				const auto validTexture = [](const MaterialTextureProperties& properties,
					MaterialTextureSlot slot) {
					return properties.texCoord <=
						static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
						finite(properties.scalar) &&
						(slot != MaterialTextureSlot::occlusion ||
							(properties.scalar >= 0.0 && properties.scalar <= 1.0));
				};
				valid = valid && validTexture(value.normal, MaterialTextureSlot::normal) &&
					validTexture(value.occlusion, MaterialTextureSlot::occlusion);
				if(!valid) return;
				target.pbrMetallicRoughness.baseColorFactor.assign(
					value.properties.baseColorFactor.begin(),
					value.properties.baseColorFactor.end());
				target.pbrMetallicRoughness.metallicFactor = value.properties.metallicFactor;
				target.pbrMetallicRoughness.roughnessFactor = value.properties.roughnessFactor;
				target.emissiveFactor.assign(value.properties.emissiveFactor.begin(),
					value.properties.emissiveFactor.end());
				target.alphaCutoff = value.properties.alphaCutoff;
				target.alphaMode = value.properties.alphaMode;
				target.doubleSided = value.properties.doubleSided;
				target.normalTexture.scale = value.normal.scalar;
				target.normalTexture.texCoord = static_cast<int>(value.normal.texCoord);
				target.occlusionTexture.strength = value.occlusion.scalar;
				target.occlusionTexture.texCoord = static_cast<int>(value.occlusion.texCoord);
			}
			else if constexpr(std::is_same_v<T, MaterialTextureEdit>)
			{
				const auto& properties = value.value;
				valid = properties.texCoord <= static_cast<std::uint32_t>(std::numeric_limits<int>::max())
					&& finite(properties.scalar)
					&& (value.slot != MaterialTextureSlot::occlusion
						|| (properties.scalar >= 0.0 && properties.scalar <= 1.0))
					&& ((value.slot == MaterialTextureSlot::normal
						|| value.slot == MaterialTextureSlot::occlusion) || properties.scalar == 1.0);
				int textureIndex = -1;
				if(valid && properties.texture)
				{
					const auto index = m_document.indexOf(properties.texture->untyped());
					valid = properties.texture->authority() == m_document.documentAuthority()
						&& index && *index <= static_cast<std::size_t>(std::numeric_limits<int>::max());
					if(valid) textureIndex = static_cast<int>(*index);
				}
				if(!valid) return;
				int* texture = nullptr;
				int* coordinate = nullptr;
				double* scalar = nullptr;
				switch(value.slot)
				{
					case MaterialTextureSlot::baseColor:
						texture = &target.pbrMetallicRoughness.baseColorTexture.index;
						coordinate = &target.pbrMetallicRoughness.baseColorTexture.texCoord;
						break;
					case MaterialTextureSlot::metallicRoughness:
						texture = &target.pbrMetallicRoughness.metallicRoughnessTexture.index;
						coordinate = &target.pbrMetallicRoughness.metallicRoughnessTexture.texCoord;
						break;
					case MaterialTextureSlot::normal:
						texture = &target.normalTexture.index;
						coordinate = &target.normalTexture.texCoord;
						scalar = &target.normalTexture.scale;
						break;
					case MaterialTextureSlot::occlusion:
						texture = &target.occlusionTexture.index;
						coordinate = &target.occlusionTexture.texCoord;
						scalar = &target.occlusionTexture.strength;
						break;
					case MaterialTextureSlot::emissive:
						texture = &target.emissiveTexture.index;
						coordinate = &target.emissiveTexture.texCoord;
						break;
				}
				*texture = textureIndex;
				if(textureIndex >= 0)
				{
					*coordinate = static_cast<int>(properties.texCoord);
					if(scalar) *scalar = properties.scalar;
				}
			}
			else
			{
				if(value.name.empty() || value.name.size() > m_limits.maxStringBytes)
				{
					valid = false;
					return;
				}
				if(!value.value)
				{
					target.extensions.erase(value.name);
					return;
				}
				std::size_t count = 0;
				tinygltf::Value converted;
				valid = extensionValueToTiny(*value.value, converted, m_limits, 0u, count);
				if(valid) target.extensions[value.name] = std::move(converted);
			}
		}, edit);
		if(!valid) return std::nullopt;
		if(!collectMaterialSnapshotReferences(
			m_document, target, impl->stableReferences)) return std::nullopt;
		const auto bytes = materialSnapshotBytes(target, impl->stableReferences);
		if(!bytes) return std::nullopt;
		impl->bytes = *bytes;
		MaterialSnapshot result;
		result.m_impl = std::move(impl);
		return result;
	}
	catch(...)
	{
		return std::nullopt;
	}
}

std::optional<LightSnapshot> SceneCommands::prepareLightSnapshot(
	LightId light, const LightProperties& properties) const
{
	if(!validLightProperties(properties)) return std::nullopt;
	auto current = lightSnapshot(light);
	if(!current) return std::nullopt;
	try
	{
		auto impl = std::make_shared<LightSnapshot::Impl>(*current->m_impl);
		applyLightProperties(impl->record, properties);
		const auto bytes = lightRecordBytes(impl->record);
		if(!bytes) return std::nullopt;
		impl->bytes = *bytes;
		LightSnapshot result;
		result.m_impl = std::move(impl);
		return result;
	}
	catch(...)
	{
		return std::nullopt;
	}
}

MutationResult SceneCommands::setMaterialSnapshot(
	MaterialId material, const MaterialSnapshot& snapshot, const MutationGuard& guard)
{
	// Scalar/color edits preserve object registries, topology, and payloads.
	// Publish them in place instead of cloning the complete authority model.
	if(snapshot.valid() && material.authority() == m_document.documentAuthority() &&
		snapshot.authority() == m_document.documentAuthority() &&
		snapshot.id() == material &&
		snapshot.snapshotBytes() <= m_limits.maxHistoryEntrySnapshotBytes)
	{
		const auto index = m_document.indexOf(material.untyped());
		if(index)
		{
			const auto& current = detail::SceneCommandsAccess::model(m_document)
				.materials[*index];
		if(snapshotReferencesMatch(m_document, snapshot.m_impl->stableReferences) &&
				 current.pbrMetallicRoughness.baseColorFactor.size() == 4u &&
				snapshot.m_impl->record.pbrMetallicRoughness.baseColorFactor.size() == 4u &&
				sameMaterialSnapshotExceptProperties(current, snapshot.m_impl->record))
				return mutateMaterialPropertiesOnly(
					m_document, material, snapshot.m_impl->record,
					snapshot.m_impl->stableReferences, snapshot.snapshotBytes(), guard,
					m_limits,
					m_faultInjector, m_serial);
		}
	}
	return mutate(m_document, HistoryPolicy::undoable, guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan&, std::string& message)
		{
			if(!snapshot.valid()) return MutationErrorCode::invalidSnapshot;
			if(material.authority() != m_document.documentAuthority()
				|| snapshot.authority() != m_document.documentAuthority())
				return MutationErrorCode::crossDocument;
			const auto index = m_document.indexOf(material.untyped());
			if(!index) return MutationErrorCode::staleId;
			if(snapshot.id() != material) return MutationErrorCode::invalidSnapshot;
			if(snapshot.snapshotBytes() > m_limits.maxHistoryEntrySnapshotBytes)
				return MutationErrorCode::snapshotBudgetExceeded;
			if(!snapshotReferencesMatch(m_document, snapshot.m_impl->stableReferences))
				return MutationErrorCode::invalidSnapshot;
			if(sameMaterialSnapshotRecord(model.materials[*index], snapshot.m_impl->record))
				return MutationErrorCode::noOp;
			std::vector<std::string> changedNames;
			changedNames.reserve(model.materials[*index].extensions.size()
				+ snapshot.m_impl->record.extensions.size());
			for(const auto& [name, value] : model.materials[*index].extensions)
			{
				(void)value;
				changedNames.push_back(name);
			}
			for(const auto& [name, value] : snapshot.m_impl->record.extensions)
			{
				(void)value;
				if(std::find(changedNames.begin(), changedNames.end(), name) == changedNames.end())
					changedNames.push_back(name);
			}
			model.materials[*index] = snapshot.m_impl->record;
			for(const std::string& name : changedNames)
			{
				if(model.materials[*index].extensions.contains(name))
					ensureExtensionUsed(model, name);
				else removeMaterialExtensionUsedIfUnreferenced(model, name);
			}
			message = "complete material snapshot applied";
			return MutationErrorCode::none;
		});
}

MutationResult SceneCommands::setLightSnapshot(
	LightId light, const LightSnapshot& snapshot, const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::undoable, guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan&, std::string& message)
		{
			if(!snapshot.valid()) return MutationErrorCode::invalidSnapshot;
			if(light.authority() != m_document.documentAuthority()
				|| snapshot.authority() != m_document.documentAuthority())
				return MutationErrorCode::crossDocument;
			const auto index = m_document.indexOf(light.untyped());
			if(!index) return MutationErrorCode::staleId;
			if(snapshot.id() != light) return MutationErrorCode::invalidSnapshot;
			if(snapshot.snapshotBytes() > m_limits.maxHistoryEntrySnapshotBytes)
				return MutationErrorCode::snapshotBudgetExceeded;
			if(sameLightSnapshotRecord(model.lights[*index], snapshot.m_impl->record))
				return MutationErrorCode::noOp;
			model.lights[*index] = snapshot.m_impl->record;
			message = "complete light snapshot applied";
			return MutationErrorCode::none;
		});
}

PreparedValueValidation SceneCommands::validateNodeTransform(
	NodeId node, const NodeTransform& value, MutationToken expected) const noexcept
{
	if(node.authority() != m_document.documentAuthority())
		return {PreparedValueStatus::foreignAuthority, MutationErrorCode::crossDocument};
	if(!m_document.contains(node.untyped()))
		return {PreparedValueStatus::staleId, MutationErrorCode::staleId};
	if(expected != token())
		return {expected.authority != m_document.documentAuthority()
				? PreparedValueStatus::foreignAuthority
				: PreparedValueStatus::staleTransaction,
			expected.authority != m_document.documentAuthority()
				? MutationErrorCode::crossDocument
				: MutationErrorCode::staleTransaction};
	const auto current = nodeTransform(node);
	if(!current || *current != value)
		return {PreparedValueStatus::valueMismatch, MutationErrorCode::staleTransaction};
	return {PreparedValueStatus::matches, MutationErrorCode::none};
}

PreparedValueValidation SceneCommands::validateMaterialSnapshot(
	MaterialId material, const MaterialSnapshot& value, MutationToken expected) const noexcept
{
	if(!value.valid() || value.id() != material)
		return {PreparedValueStatus::invalidSnapshot, MutationErrorCode::invalidSnapshot};
	if(material.authority() != m_document.documentAuthority()
		|| value.authority() != m_document.documentAuthority())
		return {PreparedValueStatus::foreignAuthority, MutationErrorCode::crossDocument};
	const auto index = m_document.indexOf(material.untyped());
	if(!index) return {PreparedValueStatus::staleId, MutationErrorCode::staleId};
	if(expected != token())
		return {expected.authority != m_document.documentAuthority()
				? PreparedValueStatus::foreignAuthority
				: PreparedValueStatus::staleTransaction,
			expected.authority != m_document.documentAuthority()
				? MutationErrorCode::crossDocument
				: MutationErrorCode::staleTransaction};
	if(!snapshotReferencesMatch(m_document, value.m_impl->stableReferences)
		|| !sameMaterialSnapshotRecord(
			detail::SceneCommandsAccess::model(m_document).materials[*index], value.m_impl->record))
		return {PreparedValueStatus::valueMismatch, MutationErrorCode::staleTransaction};
	return {PreparedValueStatus::matches, MutationErrorCode::none};
}

PreparedValueValidation SceneCommands::validateLightSnapshot(
	LightId light, const LightSnapshot& value, MutationToken expected) const noexcept
{
	if(!value.valid() || value.id() != light)
		return {PreparedValueStatus::invalidSnapshot, MutationErrorCode::invalidSnapshot};
	if(light.authority() != m_document.documentAuthority()
		|| value.authority() != m_document.documentAuthority())
		return {PreparedValueStatus::foreignAuthority, MutationErrorCode::crossDocument};
	const auto index = m_document.indexOf(light.untyped());
	if(!index) return {PreparedValueStatus::staleId, MutationErrorCode::staleId};
	if(expected != token())
		return {expected.authority != m_document.documentAuthority()
				? PreparedValueStatus::foreignAuthority
				: PreparedValueStatus::staleTransaction,
			expected.authority != m_document.documentAuthority()
				? MutationErrorCode::crossDocument
				: MutationErrorCode::staleTransaction};
	if(!sameLightSnapshotRecord(
		   detail::SceneCommandsAccess::model(m_document).lights[*index], value.m_impl->record))
		return {PreparedValueStatus::valueMismatch, MutationErrorCode::staleTransaction};
	return {PreparedValueStatus::matches, MutationErrorCode::none};
}

std::optional<MaterialProperties> SceneCommands::materialProperties(MaterialId material) const
{
	const auto index = m_document.indexOf(material.untyped());
	if(!index || material.authority() != m_document.documentAuthority()) return std::nullopt;
	const tinygltf::Material& source =
		detail::SceneCommandsAccess::model(m_document).materials[*index];
	if(source.pbrMetallicRoughness.baseColorFactor.size() != 4
		|| source.emissiveFactor.size() != 3) return std::nullopt;
	MaterialProperties result;
	std::copy_n(source.pbrMetallicRoughness.baseColorFactor.begin(), 4, result.baseColorFactor.begin());
	result.metallicFactor = source.pbrMetallicRoughness.metallicFactor;
	result.roughnessFactor = source.pbrMetallicRoughness.roughnessFactor;
	std::copy_n(source.emissiveFactor.begin(), 3, result.emissiveFactor.begin());
	result.alphaCutoff = source.alphaCutoff;
	result.alphaMode = source.alphaMode;
	result.doubleSided = source.doubleSided;
	return result;
}

MutationResult SceneCommands::setMaterialTexture(
	MaterialId material, MaterialTextureSlot slot,
	const MaterialTextureProperties& properties, const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::undoable,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan&, std::string& message)
	{
		if(material.authority() != m_document.documentAuthority())
			return MutationErrorCode::crossDocument;
		const auto materialIndex = m_document.indexOf(material.untyped());
		if(!materialIndex) return MutationErrorCode::staleId;
		if(properties.texCoord > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
			|| !finite(properties.scalar)) return MutationErrorCode::invalidReference;
		if(slot == MaterialTextureSlot::occlusion
			&& (properties.scalar < 0.0 || properties.scalar > 1.0))
			return MutationErrorCode::invalidReference;
		if(slot != MaterialTextureSlot::normal && slot != MaterialTextureSlot::occlusion
			&& properties.scalar != 1.0) return MutationErrorCode::invalidReference;
		int textureIndex = -1;
		if(properties.texture)
		{
			if(properties.texture->authority() != m_document.documentAuthority())
				return MutationErrorCode::crossDocument;
			const auto index = m_document.indexOf(properties.texture->untyped());
			if(!index) return MutationErrorCode::staleId;
			if(*index > static_cast<std::size_t>(std::numeric_limits<int>::max()))
				return MutationErrorCode::limitExceeded;
			textureIndex = static_cast<int>(*index);
		}
		auto& target = model.materials[*materialIndex];
		int* index = nullptr;
		int* texCoord = nullptr;
		double* scalar = nullptr;
		switch(slot)
		{
			case MaterialTextureSlot::baseColor:
				index = &target.pbrMetallicRoughness.baseColorTexture.index;
				texCoord = &target.pbrMetallicRoughness.baseColorTexture.texCoord;
				break;
			case MaterialTextureSlot::metallicRoughness:
				index = &target.pbrMetallicRoughness.metallicRoughnessTexture.index;
				texCoord = &target.pbrMetallicRoughness.metallicRoughnessTexture.texCoord;
				break;
			case MaterialTextureSlot::normal:
				index = &target.normalTexture.index;
				texCoord = &target.normalTexture.texCoord;
				scalar = &target.normalTexture.scale;
				break;
			case MaterialTextureSlot::occlusion:
				index = &target.occlusionTexture.index;
				texCoord = &target.occlusionTexture.texCoord;
				scalar = &target.occlusionTexture.strength;
				break;
			case MaterialTextureSlot::emissive:
				index = &target.emissiveTexture.index;
				texCoord = &target.emissiveTexture.texCoord;
				break;
		}
		const int coordinate = static_cast<int>(properties.texCoord);
		if(textureIndex < 0 && *index < 0) return MutationErrorCode::noOp;
		if(*index == textureIndex && *texCoord == coordinate
			&& (scalar == nullptr || *scalar == properties.scalar))
			return MutationErrorCode::noOp;
		*index = textureIndex;
		if(textureIndex >= 0)
		{
			*texCoord = coordinate;
			if(scalar != nullptr) *scalar = properties.scalar;
		}
		message = "material texture slot updated";
		return MutationErrorCode::none;
	});
}

std::optional<MaterialTextureProperties> SceneCommands::materialTexture(
	MaterialId material, MaterialTextureSlot slot) const
{
	const auto materialIndex = m_document.indexOf(material.untyped());
	if(!materialIndex || material.authority() != m_document.documentAuthority()) return std::nullopt;
	const auto& source = detail::SceneCommandsAccess::model(m_document).materials[*materialIndex];
	int index = -1;
	int texCoord = 0;
	double scalar = 1.0;
	switch(slot)
	{
		case MaterialTextureSlot::baseColor:
			index = source.pbrMetallicRoughness.baseColorTexture.index;
			texCoord = source.pbrMetallicRoughness.baseColorTexture.texCoord;
			break;
		case MaterialTextureSlot::metallicRoughness:
			index = source.pbrMetallicRoughness.metallicRoughnessTexture.index;
			texCoord = source.pbrMetallicRoughness.metallicRoughnessTexture.texCoord;
			break;
		case MaterialTextureSlot::normal:
			index = source.normalTexture.index;
			texCoord = source.normalTexture.texCoord;
			scalar = source.normalTexture.scale;
			break;
		case MaterialTextureSlot::occlusion:
			index = source.occlusionTexture.index;
			texCoord = source.occlusionTexture.texCoord;
			scalar = source.occlusionTexture.strength;
			break;
		case MaterialTextureSlot::emissive:
			index = source.emissiveTexture.index;
			texCoord = source.emissiveTexture.texCoord;
			break;
	}
	if(texCoord < 0) return std::nullopt;
	MaterialTextureProperties result;
	result.texCoord = static_cast<std::uint32_t>(texCoord);
	result.scalar = scalar;
	if(index >= 0)
	{
		result.texture = TextureId::from(m_document.idAt(
			EditorObjectKind::texture, static_cast<std::size_t>(index)));
		if(!result.texture) return std::nullopt;
	}
	return result;
}

MutationResult SceneCommands::setMaterialExtension(
	MaterialId material, std::string name,
	std::optional<MaterialExtensionValue> value, const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::undoable,
		guard, m_limits, m_faultInjector, m_serial,
		[&, name = std::move(name)](tinygltf::Model& model, Plan&, std::string& message)
		mutable
	{
		if(material.authority() != m_document.documentAuthority())
			return MutationErrorCode::crossDocument;
		const auto index = m_document.indexOf(material.untyped());
		if(!index) return MutationErrorCode::staleId;
		if(name.empty() || name.size() > m_limits.maxStringBytes)
			return MutationErrorCode::invalidExtension;
		auto& extensions = model.materials[*index].extensions;
		const auto existing = extensions.find(name);
		if(!value)
		{
			if(existing == extensions.end()) return MutationErrorCode::noOp;
			extensions.erase(existing);
			removeMaterialExtensionUsedIfUnreferenced(model, name);
			message = "material extension removed";
			return MutationErrorCode::none;
		}
		tinygltf::Value converted;
		std::size_t valueCount = 0;
		if(!extensionValueToTiny(*value, converted, m_limits, 0, valueCount))
			return MutationErrorCode::invalidExtension;
		if(existing != extensions.end() && existing->second == converted)
			return MutationErrorCode::noOp;
		extensions[name] = std::move(converted);
		ensureExtensionUsed(model, name);
		message = "material extension updated";
		return MutationErrorCode::none;
	});
}

std::optional<MaterialExtensionValue> SceneCommands::materialExtension(
	MaterialId material, std::string_view name) const
{
	const auto index = m_document.indexOf(material.untyped());
	if(!index || material.authority() != m_document.documentAuthority()) return std::nullopt;
	const auto& extensions = detail::SceneCommandsAccess::model(m_document).materials[*index].extensions;
	const auto found = extensions.find(std::string(name));
	if(found == extensions.end()) return std::nullopt;
	MaterialExtensionValue result;
	std::size_t valueCount = 0;
	if(!extensionValueFromTiny(found->second, result, m_limits, 0, valueCount)) return std::nullopt;
	return result;
}

std::optional<MeshId> SceneCommands::nodeMesh(NodeId node) const
{
	const auto nodeIndex = m_document.indexOf(node.untyped());
	if(!nodeIndex || node.authority() != m_document.documentAuthority()) return std::nullopt;
	const int mesh = detail::SceneCommandsAccess::model(m_document).nodes[*nodeIndex].mesh;
	if(mesh < 0 || static_cast<std::size_t>(mesh)
		>= m_document.objectCount(EditorObjectKind::mesh)) return std::nullopt;
	return MeshId::from(m_document.idAt(
		EditorObjectKind::mesh, static_cast<std::size_t>(mesh)));
}

std::vector<PrimitiveId> SceneCommands::meshPrimitives(MeshId mesh) const
{
	std::vector<PrimitiveId> result;
	const auto meshIndex = m_document.indexOf(mesh.untyped());
	if(!meshIndex || mesh.authority() != m_document.documentAuthority()) return result;
	const auto& model = detail::SceneCommandsAccess::model(m_document);
	if(*meshIndex >= model.meshes.size()) return result;
	result.reserve(model.meshes[*meshIndex].primitives.size());
	for(std::size_t primitive = 0;
		primitive < model.meshes[*meshIndex].primitives.size(); ++primitive)
	{
		if(const auto id = m_document.primitiveIdAt(PrimitiveAddress{
			static_cast<std::uint32_t>(*meshIndex), static_cast<std::uint32_t>(primitive)}))
		{
			result.push_back(*id);
		}
	}
	return result;
}

std::optional<MaterialId> SceneCommands::primitiveMaterial(PrimitiveId primitive) const
{
	if(primitive.authority() != m_document.documentAuthority()) return std::nullopt;
	const auto address = m_document.primitiveAddress(primitive);
	const auto& model = detail::SceneCommandsAccess::model(m_document);
	if(!address || address->meshIndex >= model.meshes.size()
		|| address->primitiveIndex >= model.meshes[address->meshIndex].primitives.size())
		return std::nullopt;
	const int material = model.meshes[address->meshIndex]
		.primitives[address->primitiveIndex].material;
	if(material < 0 || static_cast<std::size_t>(material) >= model.materials.size())
		return std::nullopt;
	return MaterialId::from(m_document.idAt(
		EditorObjectKind::material, static_cast<std::size_t>(material)));
}

std::size_t SceneCommands::meshReferenceCount(MeshId mesh) const noexcept
{
	const auto meshIndex = m_document.indexOf(mesh.untyped());
	if(!meshIndex || mesh.authority() != m_document.documentAuthority()) return 0u;
	return meshUseCount(
		detail::SceneCommandsAccess::model(m_document), *meshIndex);
}

std::size_t SceneCommands::materialReferenceCount(MaterialId material) const noexcept
{
	const auto materialIndex = m_document.indexOf(material.untyped());
	if(!materialIndex || material.authority() != m_document.documentAuthority()) return 0u;
	std::size_t result = 0u;
	for(const auto& mesh : detail::SceneCommandsAccess::model(m_document).meshes)
		for(const auto& primitive : mesh.primitives)
			if(primitive.material == static_cast<int>(*materialIndex)) ++result;
	return result;
}

MutationResult SceneCommands::duplicateMaterial(MaterialId material,
	const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::nonHistory,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan& plan, std::string& message)
	{
		if(material.authority() != m_document.documentAuthority())
			return MutationErrorCode::crossDocument;
		const auto index = m_document.indexOf(material.untyped());
		if(!index) return MutationErrorCode::staleId;
		if(*index >= model.materials.size()) return MutationErrorCode::staleId;
		if(model.materials.size() >= m_limits.maxMaterials)
			return MutationErrorCode::limitExceeded;
		tinygltf::Material copy = model.materials[*index];
		copy.name += "_copy";
		model.materials.push_back(std::move(copy));
		plan.idsByKind[kindIndex(EditorObjectKind::material)].push_back(
			EditorObjectId{});
		message = "material duplicated";
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::assignPrimitiveMaterial(PrimitiveId primitive, std::optional<MaterialId> material,
	const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::nonHistory,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan&, std::string&)
	{
		const auto index = m_document.indexOf(primitive.untyped());
		if(!index) return primitive.authority() != m_document.documentAuthority() ? MutationErrorCode::crossDocument : MutationErrorCode::staleId;
		const auto address = m_document.primitiveAddress(primitive);
		if(!address || address->meshIndex >= model.meshes.size()) return MutationErrorCode::staleId;
		int materialIndex = -1;
		if(material)
		{
			const auto materialValue = m_document.indexOf(material->untyped());
			if(!materialValue) return material->authority() != m_document.documentAuthority() ? MutationErrorCode::crossDocument : MutationErrorCode::staleId;
			materialIndex = static_cast<int>(*materialValue);
		}
		auto& target = model.meshes[address->meshIndex].primitives[address->primitiveIndex];
		if(target.material == materialIndex) return MutationErrorCode::noOp;
		target.material = materialIndex;
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::duplicatePrimitive(PrimitiveId primitive, const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::nonHistory,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan& plan, std::string&)
	{
		if(primitive.authority() != m_document.documentAuthority())
			return MutationErrorCode::crossDocument;
		const auto address = m_document.primitiveAddress(primitive);
		if(!address || address->meshIndex >= model.meshes.size() || address->primitiveIndex >= model.meshes[address->meshIndex].primitives.size()) return primitive.authority() != m_document.documentAuthority() ? MutationErrorCode::crossDocument : MutationErrorCode::staleId;
		std::size_t primitiveCount = 0;
		for(const auto& mesh : model.meshes) primitiveCount += mesh.primitives.size();
		if(primitiveCount >= m_limits.maxPrimitives) return MutationErrorCode::limitExceeded;
		const auto oldAddresses = plan.primitiveAddresses;
		const auto oldIds = plan.idsByKind[kindIndex(EditorObjectKind::primitive)];
		model.meshes[address->meshIndex].primitives.push_back(model.meshes[address->meshIndex].primitives[address->primitiveIndex]);
		rebuildPrimitivePlan(plan, oldAddresses, oldIds, model);
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::splitPrimitive(
	NodeId node, PrimitiveId primitive, const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::nonHistory,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan& plan, std::string&)
	{
		if(node.authority() != m_document.documentAuthority()
			|| primitive.authority() != m_document.documentAuthority())
			return MutationErrorCode::crossDocument;
		const auto nodeIndex = m_document.indexOf(node.untyped());
		const auto address = m_document.primitiveAddress(primitive);
		if(!nodeIndex || !address || address->meshIndex >= model.meshes.size()
			|| address->primitiveIndex
				>= model.meshes[address->meshIndex].primitives.size())
			return MutationErrorCode::staleId;
		if(model.nodes[*nodeIndex].mesh != static_cast<int>(address->meshIndex))
			return MutationErrorCode::invalidReference;
		const int material = model.meshes[address->meshIndex]
			.primitives[address->primitiveIndex].material;
		if(material < 0) return MutationErrorCode::noOp;
		if(static_cast<std::size_t>(material) >= model.materials.size())
			return MutationErrorCode::invalidReference;
		const std::size_t copiedPrimitiveCount =
			model.meshes[address->meshIndex].primitives.size();
		std::size_t primitiveCount = 0u;
		for(const auto& mesh : model.meshes) primitiveCount += mesh.primitives.size();
		if(model.meshes.size() >= m_limits.maxMeshes
			|| model.materials.size() >= m_limits.maxMaterials
			|| primitiveCount > m_limits.maxPrimitives
			|| copiedPrimitiveCount > m_limits.maxPrimitives - primitiveCount)
			return MutationErrorCode::limitExceeded;

		auto idsByMesh = primitiveIdsByMesh(plan, model.meshes.size());
		tinygltf::Mesh mesh = model.meshes[address->meshIndex];
		mesh.name += "_copy";
		tinygltf::Material materialCopy = model.materials[static_cast<std::size_t>(material)];
		materialCopy.name += "_copy";
		const std::size_t newMeshIndex = model.meshes.size();
		const std::size_t newMaterialIndex = model.materials.size();
		mesh.primitives[address->primitiveIndex].material =
			static_cast<int>(newMaterialIndex);
		model.meshes.push_back(std::move(mesh));
		model.materials.push_back(std::move(materialCopy));
		model.nodes[*nodeIndex].mesh = static_cast<int>(newMeshIndex);
		plan.idsByKind[kindIndex(EditorObjectKind::mesh)].push_back(EditorObjectId{});
		plan.idsByKind[kindIndex(EditorObjectKind::material)].push_back(EditorObjectId{});
		idsByMesh.emplace_back(copiedPrimitiveCount, EditorObjectId{});
		setPrimitivePlanFromMeshIds(plan, idsByMesh);
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::mergeMeshes(NodeId node, const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::nonHistory,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan& plan, std::string&)
	{
		if(node.authority() != m_document.documentAuthority())
			return MutationErrorCode::crossDocument;
		const auto nodeIndex = m_document.indexOf(node.untyped());
		if(!nodeIndex) return MutationErrorCode::staleId;
		const int source = model.nodes[*nodeIndex].mesh;
		if(source < 0 || static_cast<std::size_t>(source) >= model.meshes.size())
			return MutationErrorCode::invalidReference;
		std::optional<std::size_t> candidate;
		for(std::size_t mesh = 0; mesh < model.meshes.size(); ++mesh)
		{
			if(mesh == static_cast<std::size_t>(source)
				|| meshUseCount(model, mesh) == 0u) continue;
			if(equivalentMeshGeometry(
				model.meshes[static_cast<std::size_t>(source)], model.meshes[mesh]))
			{
				candidate = mesh;
				break;
			}
		}
		if(!candidate) return MutationErrorCode::noOp;
		model.nodes[*nodeIndex].mesh = static_cast<int>(*candidate);
		if(meshUseCount(
			model, static_cast<std::size_t>(source)) == 0u)
		{
			eraseMesh(model, plan, static_cast<std::size_t>(source));
		}
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::splitPrimitive(
	PrimitiveId primitive, const MutationGuard& guard)
{
	MutationResult result = baseResult(
		HistoryPolicy::nonHistory, m_document, guard, m_serial);
	if(result.error != MutationErrorCode::none) return result;
	if(primitive.authority() != m_document.documentAuthority())
	{
		result.error = MutationErrorCode::crossDocument;
		result.message = "primitive belongs to another document";
		return result;
	}
	const auto address = m_document.primitiveAddress(primitive);
	if(!address)
	{
		result.error = MutationErrorCode::staleId;
		result.message = "primitive ID is stale";
		return result;
	}
	const auto& model = detail::SceneCommandsAccess::model(m_document);
	std::optional<std::size_t> selectedNode;
	for(std::size_t node = 0; node < model.nodes.size(); ++node)
	{
		if(model.nodes[node].mesh != static_cast<int>(address->meshIndex)) continue;
		if(selectedNode)
		{
			result.error = MutationErrorCode::multiScenePolicy;
			result.message = "shared mesh split requires an explicit selected node";
			return result;
		}
		selectedNode = node;
	}
	if(!selectedNode)
	{
		result.error = MutationErrorCode::invalidReference;
		result.message = "primitive mesh has no selecting node";
		return result;
	}
	const auto node = NodeId::from(m_document.idAt(EditorObjectKind::node, *selectedNode));
	return node ? splitPrimitive(*node, primitive, guard) : result;
}

MutationResult SceneCommands::mergeMeshes(
	MeshId target, MeshId source, const MutationGuard& guard)
{
	MutationResult result = baseResult(
		HistoryPolicy::nonHistory, m_document, guard, m_serial);
	if(result.error != MutationErrorCode::none) return result;
	if(target.authority() != m_document.documentAuthority()
		|| source.authority() != m_document.documentAuthority())
	{
		result.error = MutationErrorCode::crossDocument;
		result.message = "mesh belongs to another document";
		return result;
	}
	const auto targetIndex = m_document.indexOf(target.untyped());
	const auto sourceIndex = m_document.indexOf(source.untyped());
	if(!targetIndex || !sourceIndex)
	{
		result.error = MutationErrorCode::staleId;
		result.message = "mesh ID is stale";
		return result;
	}
	if(*targetIndex == *sourceIndex)
	{
		result.status = MutationStatus::domainNoop;
		result.error = MutationErrorCode::noOp;
		result.message = "operation has no document effect";
		return result;
	}
	const auto& authority = detail::SceneCommandsAccess::model(m_document);
	std::optional<std::size_t> selectedNode;
	for(std::size_t node = 0; node < authority.nodes.size(); ++node)
	{
		if(authority.nodes[node].mesh != static_cast<int>(*sourceIndex)) continue;
		if(selectedNode)
		{
			result.error = MutationErrorCode::multiScenePolicy;
			result.message = "shared source mesh merge requires an explicit selected node";
			return result;
		}
		selectedNode = node;
	}
	if(!selectedNode)
	{
		result.status = MutationStatus::domainNoop;
		result.error = MutationErrorCode::noOp;
		result.message = "operation has no document effect";
		return result;
	}
	return mutate(m_document, HistoryPolicy::nonHistory,
		guard, m_limits, m_faultInjector, m_serial,
		[&, selectedNode = *selectedNode](
			tinygltf::Model& model, Plan& plan, std::string&)
	{
		if(meshUseCount(model, *targetIndex) == 0u
			|| !equivalentMeshGeometry(
				model.meshes[*sourceIndex], model.meshes[*targetIndex]))
			return MutationErrorCode::noOp;
		model.nodes[selectedNode].mesh = static_cast<int>(*targetIndex);
		if(meshUseCount(model, *sourceIndex) == 0u)
			eraseMesh(model, plan, *sourceIndex);
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::setCamera(CameraId camera, const CameraProperties& properties, const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::nonHistory,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan&, std::string&)
	{
		const auto index = m_document.indexOf(camera.untyped());
		if(!index) return camera.authority() != m_document.documentAuthority() ? MutationErrorCode::crossDocument : MutationErrorCode::staleId;
		if(!finite(properties.yfov) || !finite(properties.znear) || !finite(properties.zfar) || properties.yfov <= 0.0 || properties.znear <= 0.0 || (properties.zfar != 0.0 && properties.zfar <= properties.znear)) return MutationErrorCode::invalidReference;
		if(properties.type != "perspective" && properties.type != "orthographic") return MutationErrorCode::invalidReference;
		auto& target = model.cameras[*index];
		if(properties.type == "perspective")
		{
			if(!finite(properties.aspectRatio) || properties.aspectRatio < 0.0)
				return MutationErrorCode::invalidReference;
			if(target.type == properties.type && target.perspective.yfov == properties.yfov
				&& target.perspective.znear == properties.znear
				&& target.perspective.zfar == properties.zfar
				&& target.perspective.aspectRatio == properties.aspectRatio)
				return MutationErrorCode::noOp;
			target.type = properties.type;
			target.perspective.yfov = properties.yfov;
			target.perspective.znear = properties.znear;
			target.perspective.zfar = properties.zfar;
			target.perspective.aspectRatio = properties.aspectRatio;
		}
		else
		{
			if(!finite(properties.xmag) || !finite(properties.ymag)
				|| properties.xmag == 0.0 || properties.ymag == 0.0
				|| properties.zfar <= properties.znear)
				return MutationErrorCode::invalidReference;
			if(target.type == properties.type && target.orthographic.xmag == properties.xmag
				&& target.orthographic.ymag == properties.ymag
				&& target.orthographic.znear == properties.znear
				&& target.orthographic.zfar == properties.zfar)
				return MutationErrorCode::noOp;
			target.type = properties.type;
			target.orthographic.xmag = properties.xmag;
			target.orthographic.ymag = properties.ymag;
			target.orthographic.znear = properties.znear;
			target.orthographic.zfar = properties.zfar;
		}
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::setCameraFromView(
	CameraId camera,
	NodeId node,
	const CameraProperties& properties,
	const NodeTransform& transform,
	const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::nonHistory,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan&, std::string&)
	{
		if(camera.authority() != m_document.documentAuthority()
			|| node.authority() != m_document.documentAuthority())
			return MutationErrorCode::crossDocument;
		const auto cameraIndex = m_document.indexOf(camera.untyped());
		const auto nodeIndex = m_document.indexOf(node.untyped());
		if(!cameraIndex || !nodeIndex) return MutationErrorCode::staleId;

		if(!finite(properties.yfov) || !finite(properties.znear)
			|| !finite(properties.zfar) || properties.yfov <= 0.0
			|| properties.znear <= 0.0
			|| (properties.zfar != 0.0 && properties.zfar <= properties.znear)
			|| (properties.type != "perspective" && properties.type != "orthographic"))
			return MutationErrorCode::invalidReference;
		if(properties.type == "perspective")
		{
			if(!finite(properties.aspectRatio) || properties.aspectRatio < 0.0)
				return MutationErrorCode::invalidReference;
		}
		else if(!finite(properties.xmag) || !finite(properties.ymag)
			|| properties.xmag == 0.0 || properties.ymag == 0.0
			|| properties.zfar <= properties.znear)
		{
			return MutationErrorCode::invalidReference;
		}

		if(!finite(transform.translation) || !finite(transform.rotation)
			|| !finite(transform.scale))
			return MutationErrorCode::invalidTransform;
		const double norm = std::sqrt(std::inner_product(transform.rotation.begin(),
			transform.rotation.end(), transform.rotation.begin(), 0.0));
		if(!std::isfinite(norm) || std::abs(norm - 1.0) > 1e-4)
			return MutationErrorCode::invalidTransform;

		auto& cameraTarget = model.cameras[*cameraIndex];
		const bool sameCamera = properties.type == "perspective"
			? cameraTarget.type == properties.type
				&& cameraTarget.perspective.yfov == properties.yfov
				&& cameraTarget.perspective.znear == properties.znear
				&& cameraTarget.perspective.zfar == properties.zfar
				&& cameraTarget.perspective.aspectRatio == properties.aspectRatio
			: cameraTarget.type == properties.type
				&& cameraTarget.orthographic.xmag == properties.xmag
				&& cameraTarget.orthographic.ymag == properties.ymag
				&& cameraTarget.orthographic.znear == properties.znear
				&& cameraTarget.orthographic.zfar == properties.zfar;
		auto& nodeTarget = model.nodes[*nodeIndex];
		const bool sameNode = sameTransform(nodeTarget, transform);
		if(sameCamera && sameNode) return MutationErrorCode::noOp;

		if(!sameCamera)
		{
			cameraTarget.type = properties.type;
			if(properties.type == "perspective")
			{
				cameraTarget.perspective.yfov = properties.yfov;
				cameraTarget.perspective.znear = properties.znear;
				cameraTarget.perspective.zfar = properties.zfar;
				cameraTarget.perspective.aspectRatio = properties.aspectRatio;
			}
			else
			{
				cameraTarget.orthographic.xmag = properties.xmag;
				cameraTarget.orthographic.ymag = properties.ymag;
				cameraTarget.orthographic.znear = properties.znear;
				cameraTarget.orthographic.zfar = properties.zfar;
			}
		}
		if(!sameNode)
		{
			nodeTarget.matrix.clear();
			nodeTarget.translation.assign(
				transform.translation.begin(), transform.translation.end());
			nodeTarget.rotation.assign(
				transform.rotation.begin(), transform.rotation.end());
			nodeTarget.scale.assign(transform.scale.begin(), transform.scale.end());
		}
		return MutationErrorCode::none;
	});
}

std::optional<CameraProperties> SceneCommands::cameraProperties(CameraId camera) const
{
	const auto index = m_document.indexOf(camera.untyped());
	if(!index || camera.authority() != m_document.documentAuthority()) return std::nullopt;
	const auto& source = detail::SceneCommandsAccess::model(m_document).cameras[*index];
	CameraProperties result;
	result.type = source.type;
	result.yfov = source.perspective.yfov;
	result.znear = source.type == "orthographic"
		? source.orthographic.znear : source.perspective.znear;
	result.zfar = source.type == "orthographic"
		? source.orthographic.zfar : source.perspective.zfar;
	result.aspectRatio = source.perspective.aspectRatio;
	result.xmag = source.orthographic.xmag;
	result.ymag = source.orthographic.ymag;
	return result;
}

std::optional<NodeId> SceneCommands::cameraNode(CameraId camera) const noexcept
{
	if(camera.authority() != m_document.documentAuthority()) return std::nullopt;
	const auto cameraIndex = m_document.indexOf(camera.untyped());
	if(!cameraIndex) return std::nullopt;
	const auto& model = detail::SceneCommandsAccess::model(m_document);
	for(std::size_t node = 0; node < model.nodes.size(); ++node)
	{
		if(model.nodes[node].camera != static_cast<int>(*cameraIndex)) continue;
		return NodeId::from(m_document.idAt(EditorObjectKind::node, node));
	}
	return std::nullopt;
}

MutationResult SceneCommands::setLight(LightId light, const LightProperties& properties, const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::undoable,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan&, std::string&)
	{
		const auto index = m_document.indexOf(light.untyped());
		if(!index) return light.authority() != m_document.documentAuthority() ? MutationErrorCode::crossDocument : MutationErrorCode::staleId;
		if(!validLightProperties(properties)) return MutationErrorCode::invalidReference;
		auto& target = model.lights[*index];
		if(lightPropertiesOf(target) == properties) return MutationErrorCode::noOp;
		applyLightProperties(target, properties);
		return MutationErrorCode::none;
	});
}

std::optional<LightProperties> SceneCommands::lightProperties(LightId light) const
{
	const auto index = m_document.indexOf(light.untyped());
	if(!index || light.authority() != m_document.documentAuthority()) return std::nullopt;
	return lightPropertiesOf(detail::SceneCommandsAccess::model(m_document).lights[*index]);
}

std::optional<NodeId> SceneCommands::lightNode(LightId light) const noexcept
{
	if(light.authority() != m_document.documentAuthority()) return std::nullopt;
	const auto lightIndex = m_document.indexOf(light.untyped());
	if(!lightIndex) return std::nullopt;
	const auto& model = detail::SceneCommandsAccess::model(m_document);
	for(std::size_t node = 0; node < model.nodes.size(); ++node)
	{
		if(model.nodes[node].light != static_cast<int>(*lightIndex)) continue;
		return NodeId::from(m_document.idAt(EditorObjectKind::node, node));
	}
	return std::nullopt;
}

std::optional<SkinId> SceneCommands::nodeSkin(NodeId node) const noexcept
{
	if(node.authority() != m_document.documentAuthority()) return std::nullopt;
	const auto nodeIndex = m_document.indexOf(node.untyped());
	if(!nodeIndex) return std::nullopt;
	const auto& model = detail::SceneCommandsAccess::model(m_document);
	if(model.nodes[*nodeIndex].skin < 0
		|| static_cast<std::size_t>(model.nodes[*nodeIndex].skin) >= model.skins.size())
		return std::nullopt;
	return SkinId::from(m_document.idAt(
		EditorObjectKind::skin, static_cast<std::size_t>(model.nodes[*nodeIndex].skin)));
}

MutationResult SceneCommands::createLight(
	SceneId scene, std::optional<NodeId> parent, std::string nodeName,
	std::string lightName, const LightProperties& properties, const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::undoable,
		guard, m_limits, m_faultInjector, m_serial,
		[&, nodeName = std::move(nodeName), lightName = std::move(lightName)](
			tinygltf::Model& model, Plan& plan, std::string& message) mutable
	{
		if(scene.authority() != m_document.documentAuthority())
			return MutationErrorCode::crossDocument;
		const auto sceneIndex = m_document.indexOf(scene.untyped());
		if(!sceneIndex) return MutationErrorCode::staleId;
		if(!validLightProperties(properties)) return MutationErrorCode::invalidReference;
		if(nodeName.size() > m_limits.maxStringBytes || lightName.size() > m_limits.maxStringBytes)
			return MutationErrorCode::limitExceeded;
		if(model.nodes.size() >= m_limits.maxNodes || model.lights.size() >= m_limits.maxLights)
			return MutationErrorCode::limitExceeded;
		int parentIndex = -1;
		if(parent)
		{
			if(parent->authority() != m_document.documentAuthority())
				return MutationErrorCode::crossDocument;
			const auto index = m_document.indexOf(parent->untyped());
			if(!index) return MutationErrorCode::staleId;
			const auto parentScenes = occurrences(model, static_cast<int>(*index));
			if(std::find(parentScenes.begin(), parentScenes.end(), *sceneIndex) == parentScenes.end())
				return MutationErrorCode::multiScenePolicy;
			parentIndex = static_cast<int>(*index);
		}
		tinygltf::Light light;
		light.name = std::move(lightName);
		applyLightProperties(light, properties);
		const int lightIndex = static_cast<int>(model.lights.size());
		model.lights.push_back(std::move(light));
		plan.idsByKind[kindIndex(EditorObjectKind::light)].push_back(EditorObjectId{});
		tinygltf::Node node;
		node.name = std::move(nodeName);
		node.light = lightIndex;
		const int nodeIndex = static_cast<int>(model.nodes.size());
		model.nodes.push_back(std::move(node));
		plan.idsByKind[kindIndex(EditorObjectKind::node)].push_back(EditorObjectId{});
		if(parentIndex >= 0)
			model.nodes[static_cast<std::size_t>(parentIndex)].children.push_back(nodeIndex);
		else
			model.scenes[*sceneIndex].nodes.push_back(nodeIndex);
		ensureExtensionUsed(model, "KHR_lights_punctual");
		plan.hierarchyChanged = true;
		message = "light and owning node created";
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::deleteLight(LightId light, const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::nonHistory,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan& plan, std::string& message)
	{
		if(light.authority() != m_document.documentAuthority())
			return MutationErrorCode::crossDocument;
		const auto index = m_document.indexOf(light.untyped());
		if(!index) return MutationErrorCode::staleId;
		for(auto& node : model.nodes)
		{
			if(node.light == static_cast<int>(*index)) node.light = -1;
			else if(node.light > static_cast<int>(*index)) --node.light;
		}
		model.lights.erase(model.lights.begin() + static_cast<std::ptrdiff_t>(*index));
		plan.idsByKind[kindIndex(EditorObjectKind::light)].erase(
			plan.idsByKind[kindIndex(EditorObjectKind::light)].begin()
			+ static_cast<std::ptrdiff_t>(*index));
		if(model.lights.empty())
		{
			model.extensionsUsed.erase(std::remove(model.extensionsUsed.begin(),
				model.extensionsUsed.end(), "KHR_lights_punctual"), model.extensionsUsed.end());
			model.extensionsRequired.erase(std::remove(model.extensionsRequired.begin(),
				model.extensionsRequired.end(), "KHR_lights_punctual"), model.extensionsRequired.end());
		}
		message = "light deleted and node references detached";
		return MutationErrorCode::none;
	});
}

MutationResult SceneCommands::setPrimitiveVariant(PrimitiveId primitive, std::uint32_t variant,
	std::optional<MaterialId> material, const MutationGuard& guard)
{
	return mutate(m_document, HistoryPolicy::nonHistory,
		guard, m_limits, m_faultInjector, m_serial,
		[&](tinygltf::Model& model, Plan&, std::string&)
	{
		if(primitive.authority() != m_document.documentAuthority())
			return MutationErrorCode::crossDocument;
		const auto rootVariants = model.extensions.find("KHR_materials_variants");
		if(rootVariants == model.extensions.end() || !rootVariants->second.IsObject()
			|| !rootVariants->second.Has("variants") || !rootVariants->second.Get("variants").IsArray()
			|| variant >= rootVariants->second.Get("variants").Get<tinygltf::Value::Array>().size())
		{
			return MutationErrorCode::invalidReference;
		}
		const auto address = m_document.primitiveAddress(primitive);
		if(!address || address->meshIndex >= model.meshes.size() || address->primitiveIndex >= model.meshes[address->meshIndex].primitives.size()) return MutationErrorCode::staleId;
		int materialIndex = -1;
		if(material)
		{
			const auto index = m_document.indexOf(material->untyped());
			if(!index) return material->authority() != m_document.documentAuthority() ? MutationErrorCode::crossDocument : MutationErrorCode::staleId;
			materialIndex = static_cast<int>(*index);
		}
		auto& primitiveExtensions =
			model.meshes[address->meshIndex].primitives[address->primitiveIndex].extensions;
		const auto existingExtension = primitiveExtensions.find("KHR_materials_variants");
		tinygltf::Value before = existingExtension == primitiveExtensions.end()
			? tinygltf::Value{} : existingExtension->second;
		tinygltf::Value::Object object;
		if(existingExtension != primitiveExtensions.end())
		{
			if(!existingExtension->second.IsObject()) return MutationErrorCode::invalidExtension;
			object = existingExtension->second.Get<tinygltf::Value::Object>();
		}
		auto mappingsFound = object.find("mappings");
		tinygltf::Value::Array mappings;
		if(mappingsFound != object.end())
		{
			if(!mappingsFound->second.IsArray()) return MutationErrorCode::invalidExtension;
			mappings = mappingsFound->second.Get<tinygltf::Value::Array>();
		}
		std::optional<int> existingMaterial;
		for(const auto& mapping : mappings)
		{
			if(!mapping.IsObject() || !mapping.Has("material")
				|| !mapping.Get("material").IsInt() || !mapping.Has("variants")
				|| !mapping.Get("variants").IsArray())
				return MutationErrorCode::invalidExtension;
			const auto& variants = mapping.Get("variants").Get<tinygltf::Value::Array>();
			for(const auto& item : variants)
			{
				if(!item.IsInt()) return MutationErrorCode::invalidExtension;
				if(item.Get<int>() == static_cast<int>(variant))
				{
					if(existingMaterial) return MutationErrorCode::invalidExtension;
					existingMaterial = mapping.Get("material").Get<int>();
				}
			}
		}
		if((material && existingMaterial && *existingMaterial == materialIndex)
			|| (!material && !existingMaterial)) return MutationErrorCode::noOp;
		for(auto mapping = mappings.begin(); mapping != mappings.end();)
		{
			auto& variants = mapping->Get<tinygltf::Value::Object>()["variants"]
				.Get<tinygltf::Value::Array>();
			variants.erase(std::remove_if(variants.begin(), variants.end(),
				[&](const tinygltf::Value& item)
				{
					return item.Get<int>() == static_cast<int>(variant);
				}), variants.end());
			if(variants.empty()) mapping = mappings.erase(mapping);
			else ++mapping;
		}
		if(material)
		{
			auto target = std::find_if(mappings.begin(), mappings.end(),
				[&](const tinygltf::Value& mapping)
				{
					return mapping.Get("material").Get<int>() == materialIndex;
				});
			if(target == mappings.end())
			{
				tinygltf::Value::Object mapping;
				mapping.emplace("material", tinygltf::Value(materialIndex));
				tinygltf::Value::Array variants;
				variants.emplace_back(static_cast<int>(variant));
				mapping.emplace("variants", tinygltf::Value(std::move(variants)));
				mappings.emplace_back(tinygltf::Value(std::move(mapping)));
			}
			else
			{
				target->Get<tinygltf::Value::Object>()["variants"]
					.Get<tinygltf::Value::Array>().emplace_back(static_cast<int>(variant));
			}
		}
		if(mappings.empty()) object.erase("mappings");
		else object["mappings"] = tinygltf::Value(std::move(mappings));
		if(object.empty())
		{
			if(existingExtension == primitiveExtensions.end()) return MutationErrorCode::noOp;
			primitiveExtensions.erase("KHR_materials_variants");
		}
		else
		{
			tinygltf::Value after(std::move(object));
			if(existingExtension != primitiveExtensions.end() && before == after)
				return MutationErrorCode::noOp;
			primitiveExtensions["KHR_materials_variants"] = std::move(after);
		}
		return MutationErrorCode::none;
	});
}

std::optional<MaterialId> SceneCommands::primitiveVariantMaterial(
	PrimitiveId primitive, std::uint32_t variant) const
{
	if(primitive.authority() != m_document.documentAuthority()) return std::nullopt;
	const auto address = m_document.primitiveAddress(primitive);
	const auto& model = detail::SceneCommandsAccess::model(m_document);
	if(!address || address->meshIndex >= model.meshes.size()
		|| address->primitiveIndex >= model.meshes[address->meshIndex].primitives.size())
		return std::nullopt;
	const auto& extensions =
		model.meshes[address->meshIndex].primitives[address->primitiveIndex].extensions;
	const auto extension = extensions.find("KHR_materials_variants");
	if(extension == extensions.end() || !extension->second.IsObject()
		|| !extension->second.Has("mappings") || !extension->second.Get("mappings").IsArray())
		return std::nullopt;
	for(const auto& mapping : extension->second.Get("mappings").Get<tinygltf::Value::Array>())
	{
		if(!mapping.IsObject() || !mapping.Has("material") || !mapping.Get("material").IsInt()
			|| !mapping.Has("variants") || !mapping.Get("variants").IsArray()) continue;
		const bool includes = std::any_of(
			mapping.Get("variants").Get<tinygltf::Value::Array>().begin(),
			mapping.Get("variants").Get<tinygltf::Value::Array>().end(),
			[&](const tinygltf::Value& item)
			{
				return item.IsInt() && item.Get<int>() == static_cast<int>(variant);
			});
		if(!includes) continue;
		const int material = mapping.Get("material").Get<int>();
		if(material < 0) return std::nullopt;
		return MaterialId::from(m_document.idAt(
			EditorObjectKind::material, static_cast<std::size_t>(material)));
	}
	return std::nullopt;
}

MutationResult SceneCommands::previewSceneRootTransform(SceneId scene,
	const SceneRootTransform& transform, const MutationGuard& guard)
{
	MutationResult result = baseResult(
		HistoryPolicy::nonHistory, m_document, guard, m_serial);
	if(result.error != MutationErrorCode::none) return result;
	if(scene.authority() != m_document.documentAuthority())
	{
		result.error = MutationErrorCode::crossDocument;
		result.message = "scene-root transform belongs to another document";
		return result;
	}
	if(!m_document.contains(scene.untyped()))
	{
		result.error = MutationErrorCode::staleId;
		result.message = "scene-root transform scene ID is stale";
		return result;
	}
	if(!finiteSceneRoot(transform))
	{
		result.error = MutationErrorCode::sceneRootTransformInvalid;
		result.message = "scene-root transform requires finite values, a normalized quaternion, and nonzero scale";
		return result;
	}
	const SceneRootTransformState state = m_document.sceneRootTransformState(scene);
	if(!state.frozen && sameSceneRoot(state.current, transform))
	{
		result.status = MutationStatus::domainNoop;
		result.error = MutationErrorCode::noOp;
		result.message = "scene-root transform is unchanged";
		return result;
	}
	if(m_faultInjector != nullptr && m_faultInjector->shouldFail(EditorFaultPoint::allocation))
	{
		result.error = MutationErrorCode::allocationFailed;
		result.message = "scene-root result allocation fault was injected";
		return result;
	}
	if(state.externalStateSerial == std::numeric_limits<std::uint64_t>::max())
	{
		result.error = MutationErrorCode::externalStateSerialExhausted;
		result.message = "scene external-state serial is exhausted";
		return result;
	}
	std::optional<SceneRootStateTransition> transition;
	if(m_document.activeSceneId() == std::optional<SceneId>{scene})
	{
		transition = SceneRootStateTransition{
			m_document.documentAuthority(), scene, m_document.revision(),
			m_document.sceneActivationEpoch(), state.externalStateSerial,
			state.externalStateSerial + 1u, state.current, transform};
	}
	const std::uint64_t nextSerial = allocateMutationSerial();
	if(nextSerial == 0)
	{
		result.error = MutationErrorCode::internalError;
		result.message = "mutation serial space is exhausted";
		return result;
	}
	try
	{
		result.message = state.frozen
			? "scene-root transform baseline rebuilt and preview applied (non-history)"
			: "scene-root transform preview applied (non-history)";
	}
	catch(const std::bad_alloc&)
	{
		result.error = MutationErrorCode::allocationFailed;
		result.message.clear();
		return result;
	}
	const detail::MutationCommitStatus rootStatus =
		m_document.setSceneRootTransform(scene, transform);
	if(rootStatus != detail::MutationCommitStatus::applied)
	{
		result.error = rootStatus == detail::MutationCommitStatus::externalStateSerialExhausted
			? MutationErrorCode::externalStateSerialExhausted
			: MutationErrorCode::internalError;
		result.message = rootStatus == detail::MutationCommitStatus::externalStateSerialExhausted
			? "scene external-state serial is exhausted"
			: "scene-root transform could not be applied";
		return result;
	}
	result.status = MutationStatus::applied;
	result.error = MutationErrorCode::none;
	m_serial = nextSerial;
	result.transactionToken = MutationToken{
		m_document.documentAuthority(), m_document.revision(), m_serial};
	result.rootStateTransition = std::move(transition);
	return result;
}

MutationResult SceneCommands::resetSceneRootTransform(SceneId scene, const MutationGuard& guard)
{
	MutationResult result = baseResult(
		HistoryPolicy::nonHistory, m_document, guard, m_serial);
	if(result.error != MutationErrorCode::none) return result;
	if(scene.authority() != m_document.documentAuthority())
	{
		result.error = MutationErrorCode::crossDocument;
		result.message = "scene-root reset belongs to another document";
		return result;
	}
	if(!m_document.contains(scene.untyped()))
	{
		result.error = MutationErrorCode::staleId;
		result.message = "scene-root reset scene ID is stale";
		return result;
	}
	const SceneRootTransformState state = m_document.sceneRootTransformState(scene);
	if(!state.frozen && sameSceneRoot(state.current, state.baseline))
	{
		result.status = MutationStatus::domainNoop;
		result.error = MutationErrorCode::noOp;
		result.message = "scene-root transform is already at baseline";
		return result;
	}
	if(m_faultInjector != nullptr && m_faultInjector->shouldFail(EditorFaultPoint::allocation))
	{
		result.error = MutationErrorCode::allocationFailed;
		result.message = "scene-root result allocation fault was injected";
		return result;
	}
	if(state.externalStateSerial == std::numeric_limits<std::uint64_t>::max())
	{
		result.error = MutationErrorCode::externalStateSerialExhausted;
		result.message = "scene external-state serial is exhausted";
		return result;
	}
	std::optional<SceneRootStateTransition> transition;
	if(m_document.activeSceneId() == std::optional<SceneId>{scene})
	{
		transition = SceneRootStateTransition{
			m_document.documentAuthority(), scene, m_document.revision(),
			m_document.sceneActivationEpoch(), state.externalStateSerial,
			state.externalStateSerial + 1u, state.current, state.baseline};
	}
	const std::uint64_t nextSerial = allocateMutationSerial();
	if(nextSerial == 0)
	{
		result.error = MutationErrorCode::internalError;
		result.message = "mutation serial space is exhausted";
		return result;
	}
	try
	{
		result.message = state.frozen
			? "scene-root transform baseline rebuilt (non-history)"
			: "scene-root transform reset (non-history)";
	}
	catch(const std::bad_alloc&)
	{
		result.error = MutationErrorCode::allocationFailed;
		result.message.clear();
		return result;
	}
	const detail::MutationCommitStatus rootStatus = m_document.resetSceneRootTransform(scene);
	if(rootStatus != detail::MutationCommitStatus::applied)
	{
		result.error = rootStatus == detail::MutationCommitStatus::externalStateSerialExhausted
			? MutationErrorCode::externalStateSerialExhausted
			: MutationErrorCode::internalError;
		result.message = rootStatus == detail::MutationCommitStatus::externalStateSerialExhausted
			? "scene external-state serial is exhausted"
			: "scene-root transform could not be reset";
		return result;
	}
	result.status = MutationStatus::applied;
	result.error = MutationErrorCode::none;
	m_serial = nextSerial;
	result.transactionToken = MutationToken{
		m_document.documentAuthority(), m_document.revision(), m_serial};
	result.rootStateTransition = std::move(transition);
	return result;
}

} // namespace demo::editor
