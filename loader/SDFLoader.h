#pragma once

// Mesh SDF asset loader (DDGI Wave D1-1).
//
// Parses the .bin asset produced by tools/sdf_baker and creates an R16F 3D
// texture through the RHI (rhi::Device only — no native graphics types).
//
// Binary layout (.bin) — MUST stay in sync with tools/sdf_baker/SDFBaker.h:
//   offset  0: char     magic[4]      = "MSDF"
//   offset  4: uint32   version       = 1
//   offset  8: uint32   resolution[3]   (x, y, z voxel counts)
//   offset 20: float    boundsMin[3]    (padded world-space AABB)
//   offset 32: float    boundsMax[3]
//   offset 44: uint16   payload[x*y*z]  (R16F, normalized (d+1)/2, x-major)

#include "../rhi/RHICommandBuffer.h"
#include "../rhi/RHIDevice.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace demo
{
	struct MeshSDFAsset
	{
		// Padded world-space AABB the volume spans (object/local space of the
		// baked mesh; instance transforms are applied by later DDGI waves).
		glm::vec3 worldBoundsMin{0.0f};
		glm::vec3 worldBoundsMax{0.0f};
		glm::uvec3 resolution{0u};
		// R16F Texture3D holding the normalized distance field (d + 1) / 2.
		rhi::TextureHandle normalizedSDFHandle{};
		bool isValid{false};
	};

	struct SDFLoadResult
	{
		MeshSDFAsset asset{};
		std::string errorMessage;
	};

	class SDFLoader
	{
	public:
		// CPU-side parse result; useful for validation without a GPU device.
		struct FileData
		{
			glm::uvec3 resolution{0u};
			glm::vec3 boundsMin{0.0f};
			glm::vec3 boundsMax{0.0f};
			std::vector<uint16_t> halfTexels;
		};

		// Parses the .bin payload only (no GPU resources created).
		[[nodiscard]] static bool parseFile(const std::filesystem::path& path, FileData& outData,
		                                    std::string& outError);

		// Parses the .bin and creates + uploads the R16F Texture3D. The upload is
		// recorded on `cmd` (caller submits it, e.g. via executeImmediateUpload);
		// the staging buffer handle is appended to `outStagingBuffers` and must be
		// retired by the caller after submission (same contract as
		// RenderDevice::loadAndCreateImage).
		[[nodiscard]] static SDFLoadResult load(const std::filesystem::path& path, rhi::Device& device,
		                                        rhi::CommandBuffer& cmd,
		                                        std::vector<rhi::BufferHandle>& outStagingBuffers);
	};
} // namespace demo
