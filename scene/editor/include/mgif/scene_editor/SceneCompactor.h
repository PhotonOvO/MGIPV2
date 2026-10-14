#pragma once

#include <mgif/scene_editor/EditorLimits.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace demo::editor {

class EditorFaultInjector;
class GltfDocument;

enum class SceneCompactionCollectionKind : std::uint8_t
{
	scene = 0,
	node,
	primitive,
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
	animation,
	audioEmitter,
	audioSource,
	count,
};

enum class SceneCompactionStatus : std::uint8_t
{
	applied = 0,
	unchanged,
	invalidDocument,
	unsafeExtensionReference,
	dependencyRemapUnsupported,
	limitExceeded,
	validationFailed,
	allocationFailed,
	injectedFailure,
	generationExhausted,
	revisionExhausted,
	internalError,
};

struct SceneCompactionCollectionResult final
{
	std::size_t beforeCount{0};
	std::size_t afterCount{0};
	std::vector<std::optional<std::size_t>> oldToNew;

	[[nodiscard]] std::size_t removedCount() const noexcept
	{
		return beforeCount - afterCount;
	}

	[[nodiscard]] std::optional<std::size_t> newIndex(std::size_t oldIndex) const noexcept
	{
		return oldIndex < oldToNew.size() ? oldToNew[oldIndex] : std::nullopt;
	}
};

struct SceneCompactionResult final
{
	using Collections = std::array<SceneCompactionCollectionResult,
								   static_cast<std::size_t>(SceneCompactionCollectionKind::count)>;

	SceneCompactionStatus status{SceneCompactionStatus::internalError};
	std::uint64_t beforeRevision{0};
	std::uint64_t afterRevision{0};
	std::string message;
	Collections collections;

	[[nodiscard]] bool applied() const noexcept
	{
		return status == SceneCompactionStatus::applied;
	}

	[[nodiscard]] bool unchanged() const noexcept
	{
		return status == SceneCompactionStatus::unchanged;
	}

	[[nodiscard]] bool succeeded() const noexcept
	{
		return applied() || unchanged();
	}

	[[nodiscard]] const SceneCompactionCollectionResult&
	collection(SceneCompactionCollectionKind kind) const noexcept
	{
		return collections[static_cast<std::size_t>(kind)];
	}
};

class SceneCompactor final
{
public:
	[[nodiscard]] static SceneCompactionResult compact(GltfDocument& document,
													   const EditorLimits& limits = kDefaultEditorLimits,
													   EditorFaultInjector* faultInjector = nullptr) noexcept;
};

} // namespace demo::editor
