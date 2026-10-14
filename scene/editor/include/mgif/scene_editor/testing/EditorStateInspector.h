#pragma once

#include <mgif/scene_editor/EditorClock.h>
#include <mgif/scene_editor/GltfDocument.h>
#include <mgif/scene_editor/SceneCommands.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace demo::editor {

class EditorHistory;
struct RetainedCommandInspection;

struct ObjectSlotInspection final
{
	std::uint32_t generation{0};
	bool live{false};
	std::size_t index{0};
};

struct SceneRootInspection final
{
	SceneId scene;
	SceneRootTransform baseline;
	SceneRootTransform current;
	std::uint64_t hierarchyEpoch{0};
	std::uint64_t externalStateSerial{0};
	bool frozen{false};
	std::vector<NodeId> frozenRootIds;
};

struct DocumentInspection final
{
	std::array<std::vector<EditorObjectId>,
		static_cast<std::size_t>(EditorObjectKind::count)> idsByIndex;
	std::array<std::vector<ObjectSlotInspection>,
		static_cast<std::size_t>(EditorObjectKind::count)> slots;
	std::vector<PrimitiveAddress> primitiveAddresses;
	std::vector<SceneRootInspection> sceneRoots;
};

template <typename RetainedInspection>
struct HistoryEntryInspectionStorage final
{
	std::string description;
	std::uint64_t snapshotBytes{0};
	RetainedInspection retainedCommand;
};

using HistoryEntryInspection =
	HistoryEntryInspectionStorage<RetainedCommandInspection>;

struct HistoryInspection final
{
	std::vector<HistoryEntryInspection> undoOldestToNewest;
	std::vector<HistoryEntryInspection> redoOldestToNewest;
	std::optional<EditorTimePoint> lastContinuousPush;
	std::uint64_t retainedSnapshotBytes{0};
};

enum class ReferenceRewriteProbeStatus : std::uint8_t
{
	rewritten = 0,
	invalidTargetKind,
	invalidMap,
	invalidSourceReference,
	candidateValidationFailed,
};

struct ReferenceLocationInspection final
{
	std::string jsonPointer;
	std::int64_t before{-1};
	std::optional<std::int64_t> after;
	bool codecLocal{false};
	bool rewritten{false};
};

struct ReferenceRewriteProbeResult final
{
	ReferenceRewriteProbeStatus status{
		ReferenceRewriteProbeStatus::invalidTargetKind};
	std::vector<ReferenceLocationInspection> locations;
	std::string candidateSemanticDigest;
};

class EditorStateInspector final
{
public:
	[[nodiscard]] static DocumentInspection inspect(const GltfDocument&);
	[[nodiscard]] static HistoryInspection inspect(const EditorHistory&);
	[[nodiscard]] static ReferenceRewriteProbeResult probeReferenceRewrite(
		const GltfDocument& source,
		EditorObjectKind targetKind,
		std::span<const std::optional<std::uint32_t>> oldToNew);
};

#if defined(MGIF_EDITOR_TESTING)
enum class ExhaustionCounterKind : std::uint8_t
{
	objectGeneration = 0,
	documentRevision,
	hierarchyEpoch,
	externalStateSerial,
};

struct ExhaustionSeedRequest final
{
	ExhaustionCounterKind counter{ExhaustionCounterKind::documentRevision};
	std::optional<EditorObjectId> object;
	std::optional<SceneId> scene;
};

enum class ExhaustionSeedStatus : std::uint8_t
{
	seeded = 0,
	invalidTargetShape,
	staleOrForeignTarget,
	allocationFailed,
};

struct ExhaustionSeedResult final
{
	ExhaustionSeedStatus status{ExhaustionSeedStatus::invalidTargetShape};
	std::optional<EditorObjectId> seededObject;
	MutationToken token{};
	[[nodiscard]] bool seeded() const noexcept
	{
		return status == ExhaustionSeedStatus::seeded;
	}
};

struct CheckedCounterTransition final
{
	MutationErrorCode error{MutationErrorCode::internalError};
	std::uint64_t next{0};
	[[nodiscard]] bool canAdvance() const noexcept
	{
		return error == MutationErrorCode::none;
	}
};

class EditorStateTestAccess final
{
public:
	[[nodiscard]] static ExhaustionSeedResult seedCounterMaximum(
		GltfDocument& document,
		const ExhaustionSeedRequest& request) noexcept;
	[[nodiscard]] static CheckedCounterTransition checkedGenerationSuccessor(
		std::uint32_t current) noexcept;
	[[nodiscard]] static CheckedCounterTransition checkedRevisionSuccessor(
		std::uint64_t current) noexcept;
	[[nodiscard]] static CheckedCounterTransition checkedHierarchyEpochSuccessor(
		std::uint64_t current) noexcept;
	[[nodiscard]] static CheckedCounterTransition checkedExternalSerialSuccessor(
		std::uint64_t current) noexcept;
};
#endif

} // namespace demo::editor
