#pragma once

#include <mgif/scene_editor/EditorFaultInjection.h>
#include <mgif/scene_editor/EditorLimits.h>
#include <mgif/scene_editor/GltfDocument.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace demo::editor {

// A cross-document merge is prepared entirely on a candidate TinyGLTF model.
// The active document, stable-ID registry, selection, and renderer projection
// are changed only after the candidate has passed validation and remapping.
enum class SceneMergeStatus : std::uint8_t
{
	applied = 0,
	unchanged,
	invalidTarget,
	invalidIncoming,
	limitExceeded,
	unsupportedExtensionReference,
	validationFailed,
	allocationFailed,
	injectedFailure,
	generationExhausted,
	revisionExhausted,
	internalError,
};

struct SceneMergeResult final
{
	SceneMergeStatus status{SceneMergeStatus::internalError};
	std::uint64_t beforeRevision{0};
	std::uint64_t afterRevision{0};
	std::string message;
	std::optional<NodeId> wrapperNode;

	[[nodiscard]] bool applied() const noexcept
	{
		return status == SceneMergeStatus::applied;
	}

	[[nodiscard]] bool unchanged() const noexcept
	{
		return status == SceneMergeStatus::unchanged;
	}

	[[nodiscard]] bool succeeded() const noexcept
	{
		return applied() || unchanged();
	}
};

class SceneMerger final
{
public:
	// Merge the active/default scene of `incoming` below a newly-created wrapper
	// node in the target's current scene. All incoming standard collections,
	// KHR/EXT index-bearing fields, animations, skins, lights, and audio records
	// are appended and remapped. The incoming document is never mutated.
	[[nodiscard]] static SceneMergeResult merge(
		GltfDocument& target,
		const GltfDocument& incoming,
		std::string_view wrapperName = {},
		const EditorLimits& limits = kDefaultEditorLimits,
		EditorFaultInjector* faultInjector = nullptr) noexcept;
};

} // namespace demo::editor
