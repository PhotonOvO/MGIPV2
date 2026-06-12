#pragma once

// Offline Mesh SDF baker (DDGI Wave D1-1).
//
// Pure-CPU tool: no RHI / Vulkan / engine dependency. Loads a triangle mesh
// (self-contained Wavefront OBJ parser), evaluates a signed distance field on
// a regular voxel grid (median-split triangle BVH for closest-point queries +
// pseudo-normal back-face voting for the sign, mirroring LuxGI SDFBaker.cpp),
// normalizes distances as (d / maxDistance + 1) / 2 and writes an R16F payload
// to a .bin asset consumed by loader/SDFLoader.
//
// Binary layout (.bin) — MUST stay in sync with loader/SDFLoader.h
// (demo::sdf format constants):
//   offset  0: char     magic[4]      = "MSDF"
//   offset  4: uint32   version       = 1
//   offset  8: uint32   resolution[3]   (x, y, z voxel counts)
//   offset 20: float    boundsMin[3]    (padded world-space AABB)
//   offset 32: float    boundsMax[3]
//   offset 44: uint16   payload[x*y*z]  (R16F, normalized (d+1)/2, x-major)

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace sdf_baker
{
	inline constexpr char kSDFMagic[4] = {'M', 'S', 'D', 'F'};
	inline constexpr uint32_t kSDFVersion = 1u;

	struct SDFBakerConfig
	{
		std::string inputMeshPath;
		std::string outputPath;
		// Target voxel density: voxels per world-space meter. Per-axis resolution
		// is ceil(extent * targetTexelPerMeter), then clamped to [minResolution,
		// maxResolution]. Note: LuxGI SDFBaker.cpp:60 divides by this value
		// (meters-per-texel semantics); we multiply, matching the parameter name.
		// With the 32-64 clamp the practical difference is small.
		float targetTexelPerMeter{3.0f};
		uint32_t minResolution{32u};
		uint32_t maxResolution{64u};
		// Sign voting: sampleCount^2 sphere directions per voxel (LuxGI-style
		// back-face hit voting). 6 -> 36 rays per voxel.
		uint32_t sampleCount{6u};
	};

	struct BakedSDF
	{
		glm::uvec3 resolution{0u};
		glm::vec3 boundsMin{0.0f};
		glm::vec3 boundsMax{0.0f};
		std::vector<uint16_t> halfTexels; // R16F payload, x-major flattening
	};

	struct Mesh
	{
		std::vector<glm::vec3> positions;
		std::vector<uint32_t> indices; // triangle list
	};

	// Self-contained float -> IEEE 754 binary16 conversion (round to nearest even).
	[[nodiscard]] uint16_t floatToHalf(float value);

	// Minimal Wavefront OBJ parser: 'v' and 'f' records, fan triangulation,
	// v / v/vt / v//vn / v/vt/vn index forms, negative (relative) indices.
	[[nodiscard]] bool loadObj(const std::string& path, Mesh& outMesh, std::string& outError);

	// Bakes the SDF for the given mesh. Returns false (with outError set) when
	// the mesh is empty/degenerate.
	[[nodiscard]] bool bake(const Mesh& mesh, const SDFBakerConfig& config, BakedSDF& outSDF,
	                        std::string& outError);

	// Serializes the baked SDF to the .bin layout documented above.
	[[nodiscard]] bool writeBin(const std::string& path, const BakedSDF& sdf, std::string& outError);
} // namespace sdf_baker
