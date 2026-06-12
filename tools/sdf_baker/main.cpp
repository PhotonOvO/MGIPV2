// Offline Mesh SDF baker CLI (DDGI Wave D1-1).
//
// Usage:
//   sdf_baker <input.obj> <output.bin> [--texel-per-meter F] [--min-res N]
//             [--max-res N] [--samples N]
//
// Pure-CPU tool: no RHI / Vulkan dependency. See SDFBaker.h for the .bin layout.

#include "SDFBaker.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
	void printUsage()
	{
		std::printf(
			"Usage: sdf_baker <input.obj> <output.bin> [options]\n"
			"Options:\n"
			"  --texel-per-meter <float>  Voxel density per world meter (default 3.0)\n"
			"  --min-res <uint>           Minimum per-axis resolution (default 32)\n"
			"  --max-res <uint>           Maximum per-axis resolution (default 64)\n"
			"  --samples <uint>           Sign-voting grid size N (N*N rays/voxel, default 6)\n");
	}
} // namespace

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		printUsage();
		return EXIT_FAILURE;
	}

	sdf_baker::SDFBakerConfig config;
	config.inputMeshPath = argv[1];
	config.outputPath = argv[2];

	for (int i = 3; i < argc; ++i)
	{
		const auto requireValue = [&](const char* flag) -> const char*
		{
			if (i + 1 >= argc)
			{
				std::fprintf(stderr, "Missing value for %s\n", flag);
				std::exit(EXIT_FAILURE);
			}
			return argv[++i];
		};

		if (std::strcmp(argv[i], "--texel-per-meter") == 0)
		{
			config.targetTexelPerMeter = std::strtof(requireValue("--texel-per-meter"), nullptr);
		}
		else if (std::strcmp(argv[i], "--min-res") == 0)
		{
			config.minResolution = static_cast<uint32_t>(std::strtoul(requireValue("--min-res"), nullptr, 10));
		}
		else if (std::strcmp(argv[i], "--max-res") == 0)
		{
			config.maxResolution = static_cast<uint32_t>(std::strtoul(requireValue("--max-res"), nullptr, 10));
		}
		else if (std::strcmp(argv[i], "--samples") == 0)
		{
			config.sampleCount = static_cast<uint32_t>(std::strtoul(requireValue("--samples"), nullptr, 10));
		}
		else
		{
			std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
			printUsage();
			return EXIT_FAILURE;
		}
	}

	if (config.targetTexelPerMeter <= 0.0f || config.minResolution == 0 ||
	    config.maxResolution < config.minResolution || config.sampleCount == 0)
	{
		std::fprintf(stderr, "Invalid configuration values\n");
		return EXIT_FAILURE;
	}

	std::string error;
	sdf_baker::Mesh mesh;
	if (!sdf_baker::loadObj(config.inputMeshPath, mesh, error))
	{
		std::fprintf(stderr, "Mesh load failed: %s\n", error.c_str());
		return EXIT_FAILURE;
	}
	std::printf("Loaded %s: %zu vertices, %zu triangles\n", config.inputMeshPath.c_str(),
	            mesh.positions.size(), mesh.indices.size() / 3);

	const auto startTime = std::chrono::steady_clock::now();
	sdf_baker::BakedSDF baked;
	if (!sdf_baker::bake(mesh, config, baked, error))
	{
		std::fprintf(stderr, "Bake failed: %s\n", error.c_str());
		return EXIT_FAILURE;
	}
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - startTime);

	std::printf("Baked SDF %ux%ux%u in %lld ms, bounds (%.3f %.3f %.3f) - (%.3f %.3f %.3f)\n",
	            baked.resolution.x, baked.resolution.y, baked.resolution.z,
	            static_cast<long long>(elapsed.count()),
	            baked.boundsMin.x, baked.boundsMin.y, baked.boundsMin.z,
	            baked.boundsMax.x, baked.boundsMax.y, baked.boundsMax.z);

	if (!sdf_baker::writeBin(config.outputPath, baked, error))
	{
		std::fprintf(stderr, "Write failed: %s\n", error.c_str());
		return EXIT_FAILURE;
	}
	std::printf("Wrote %s\n", config.outputPath.c_str());
	return EXIT_SUCCESS;
}
