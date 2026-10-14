#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace demo::editor {

struct EditorLimits final
{
	std::size_t maxScenes{1u << 14};
	std::size_t maxNodes{1u << 16};
	std::size_t maxNodeEdges{1u << 22};
	std::size_t maxSceneNodeOccurrences{1u << 24};
	// Projection and runtime transform rebuilds currently use bounded native
	// recursion. Keep accepted documents below a conservative stack-safe depth;
	// maxNodes remains independent and still permits 65,536-node wide scenes.
	std::size_t maxNodeDepth{512};
	std::size_t maxMeshes{1u << 16};
	std::size_t maxPrimitives{1u << 16};
	std::size_t maxMaterials{1u << 14};
	std::size_t maxTextures{1u << 14};
	std::size_t maxImages{1u << 14};
	std::size_t maxSamplers{1u << 14};
	std::size_t maxCameras{1u << 14};
	std::size_t maxLights{1u << 14};
	std::size_t maxAccessors{1u << 16};
	std::size_t maxAccessorElements{1u << 24};
	std::size_t maxBufferViews{1u << 16};
	std::size_t maxBuffers{1u << 16};
	std::size_t maxSkins{1u << 16};
	std::size_t maxAnimations{1u << 16};
	std::size_t maxAnimationChannels{1u << 20};
	std::size_t maxSkinJoints{1u << 20};
	std::size_t maxMorphTargets{1u << 20};
	std::size_t maxAudioEmitters{1u << 14};
	std::size_t maxAudioSources{1u << 14};
	std::size_t maxDependencies{1u << 14};
	std::size_t maxImageDimension{1u << 14};
	std::size_t maxJsonDepth{256};
	std::size_t maxJsonValues{1u << 20};
	std::size_t maxStringBytes{1u << 24};
	std::size_t maxValidationIssues{64};
	std::uint64_t maxSourceBytes{1ull << 30};
	std::uint64_t maxExternalFileBytes{4ull << 30};
	std::uint64_t maxAggregateFileBytes{8ull << 30};
	std::uint64_t maxBufferBytes{4ull << 30};
	std::uint64_t maxImageBytes{1ull << 30};
	std::uint64_t maxAuthorityPayloadBytes{8ull << 30};
	std::uint64_t maxHistoryEntrySnapshotBytes{8ull * 1024ull * 1024ull};
	std::uint64_t maxHistoryAggregateSnapshotBytes{64ull * 1024ull * 1024ull};
};

inline constexpr EditorLimits kDefaultEditorLimits{};

[[nodiscard]] constexpr bool editorCheckedAdd(
	std::uint64_t left,
	std::uint64_t right,
	std::uint64_t& result) noexcept
{
	if(right > std::numeric_limits<std::uint64_t>::max() - left)
	{
		return false;
	}
	result = left + right;
	return true;
}

[[nodiscard]] constexpr bool editorCheckedMultiply(
	std::uint64_t left,
	std::uint64_t right,
	std::uint64_t& result) noexcept
{
	if(left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
	{
		return false;
	}
	result = left * right;
	return true;
}

} // namespace demo::editor
