#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace demo
{
	// Interleaved vertex layout both mesh upload paths use: the position, normal, texture
	// coordinate and tangent of one vertex sit at fixed float offsets in a 48-byte record.
	struct MeshLodVertexLayout
	{
		static constexpr size_t kPositionFloatOffset = 0u;
		static constexpr size_t kNormalFloatOffset = 3u;
		static constexpr size_t kUvFloatOffset = 6u;
	};

	// A chain holds at most this many levels, including the caller's source mesh as level 0.
	inline constexpr uint32_t kMeshLodLevelCapacity = 4u;

	// Which optional attributes the vertex data actually carries. An attribute that is absent
	// must not enter the simplification metric.
	struct MeshLodAttributes
	{
		bool normals{false};
		bool uv{false};
	};

	// Simplifications of one mesh, produced at load time with meshoptimizer.
	//
	// Level 0 is always the caller's source mesh and is not listed here. The levels below are
	// ordered coarse-last, and each one is a simplification of the level above it, so switching
	// a level can only remove detail.
	struct MeshLodChain
	{
		struct Level
		{
			// Offset into `indices`, in elements. The caller rebases it onto its own buffer.
			uint32_t indexOffset{0};
			uint32_t indexCount{0};
			// Absolute error in mesh units, so a screen-space error test can compare it against
			// a pixel threshold without knowing the mesh scale.
			float geometricError{0.0f};
		};

		// All levels concatenated. Empty when the mesh is too small to simplify, or when
		// simplification removed nothing.
		std::vector<uint32_t> indices;
		std::vector<Level> levels;
	};

	class MeshLodBuilder
	{
	public:
		// Builds up to `maxLevels` simplifications of a triangle list.
		//
		// Simplification is attribute aware: normals and texture coordinates take part in the
		// error metric, so a coarse level does not fold a texture seam or flip a normal. Weights
		// are relative to the position metric, which is always 1.
		static constexpr float kNormalAttributeWeight = 0.5f;
		static constexpr float kUvAttributeWeight = 1.0f;

		// `vertices` points at the first float of the first vertex. `vertexStride` is in bytes.
		// Borders are locked, because two meshes that meet at an edge must not crack apart when
		// only one of them switches level.
		[[nodiscard]] static MeshLodChain build(
			const float* vertices, uint32_t vertexCount, size_t vertexStride,
			const MeshLodAttributes& attributes, const uint32_t* indices, size_t indexCount,
			uint32_t maxLevels = 3);
	};
} // namespace demo
