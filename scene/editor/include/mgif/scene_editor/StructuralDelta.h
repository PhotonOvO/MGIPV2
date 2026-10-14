#pragma once

#include <mgif/scene_editor/EditorObjectId.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace demo::editor {

enum class StructuralState : std::uint8_t
{
	undo = 0,
	redo,
};

enum class StructuralDeltaInspectionStatus : std::uint8_t
{
	complete = 0,
	invalidDelta,
	invalidState,
	allocationFailed,
};

// Canonical bytes are an allocated, read-only copy. The delta never retains the
// returned buffer or charges it to snapshotBytes().
struct StructuralDeltaCanonicalSide final
{
	static constexpr std::uint32_t kSchemaVersion = 1u;

	std::uint32_t schemaVersion{kSchemaVersion};
	StructuralState side{StructuralState::undo};
	StructuralDeltaInspectionStatus status{StructuralDeltaInspectionStatus::invalidDelta};
	std::vector<std::uint8_t> canonicalBytes;

	[[nodiscard]] bool complete() const noexcept
	{
		return status == StructuralDeltaInspectionStatus::complete;
	}
};

class StructuralDelta final
{
public:
	StructuralDelta(StructuralDelta&&) noexcept;
	StructuralDelta& operator=(StructuralDelta&&) noexcept;
	~StructuralDelta();

	StructuralDelta(const StructuralDelta&) = delete;
	StructuralDelta& operator=(const StructuralDelta&) = delete;

	[[nodiscard]] bool valid() const noexcept;
	[[nodiscard]] EditorDocumentAuthority authority() const noexcept;
	[[nodiscard]] std::uint64_t snapshotBytes() const noexcept;

	// Returns a deterministic, pointer-independent side image. Schema v1 is:
	//   "MGIFSDC1", u32 version, u8 side, u32 sectionCount, then sections.
	// A section is a four-byte ASCII tag followed by its u64 payload byte length.
	// All integers and IEEE-754 binary64 bits are little-endian; i32 uses its
	// two's-complement bits; bool is u8 0 or 1. Strings, binary values, vectors,
	// lists, and sets carry a u64 byte length or element count before contents.
	// An ID is {kind:u8, slot:u32, generation:u32, authority:u64}. A reference is
	// tag 0 (absent), tag 1 plus an ID, or tag 2 plus an unresolved raw i32.
	// Value tags are 0 null, 1 bool, 2 i32, 3 f64, 4 string, 5 binary, 6 array,
	// 7 object, and 255 unknown; these tags do not expose a backend enum.
	// Ordered inputs retain order; unordered ID and string sets are sorted.
	//
	// The eight v1 sections, in order, are:
	//   DIAG authority, snapshot bytes, capture revision/serial, operation kind,
	//        side revision, and the stable scene-ID/hierarchy-epoch list.
	//   REGI all kind registries plus maximal owned runs and their complete
	//        predecessor/successor fallback chains.
	//   SLOT the sorted union of both sides' IDs with side presence/ordinal and
	//        collection, record, and node-membership ownership flags.
	//   RECD scene, node, skin, animation, and light records in registry order;
	//        each ITEM is length-delimited and all object references use IDs.
	//   MEMB every scene-root and node-child LIST, including owner ID, ordered
	//        membership, mapping validity, and the same anchored-run metadata.
	//   PLAN current scene, hierarchy/resurrection/serial flags, primitive stable
	//        ID/address order, frozen roots, and complete scene-root runtime
	//        state.
	//   OWNR collection/record ID sets for every kind and node-membership IDs.
	//   EXTN ordered Used/Required names and their sorted owned-name sets.
	// Operation kinds are 0 AddChild, 1 AddLight, 2 Duplicate, 3 Delete, and
	// 4 Reparent. Nested ITEM and LIST values use the same section framing.
	// A complete result has canonicalBytes in this format. invalidDelta,
	// invalidState, and allocationFailed return empty bytes. Inspection never
	// mutates retained state, exposes backend/native objects, or throws.
	[[nodiscard]] StructuralDeltaCanonicalSide inspectCanonicalSide(
		StructuralState side) const noexcept;

private:
	struct Impl;
	explicit StructuralDelta(std::unique_ptr<Impl>) noexcept;
	std::unique_ptr<Impl> m_impl;
	friend class SceneCommands;
};

} // namespace demo::editor
